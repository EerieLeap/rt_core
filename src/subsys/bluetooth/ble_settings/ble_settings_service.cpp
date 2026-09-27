#include <algorithm>
#include <exception>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "ble_settings_command/ble_settings_command_start_read.h"
#include "ble_settings_command/ble_settings_command_end_read.h"
#include "ble_settings_command/ble_settings_command_result.h"
#include "ble_settings_service.h"

LOG_MODULE_REGISTER(ble_settings_logger);

namespace eerie_leap::subsys::bluetooth::ble_settings {

using namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command;

#define BT_UUID_SETTINGS_CONTROL_VAL BT_UUID_SETTINGS_SERVICE_ENCODE(1)
#define BT_UUID_SETTINGS_DATA_VAL BT_UUID_SETTINGS_SERVICE_ENCODE(2)
#define BT_UUID_SETTINGS_STATUS_VAL BT_UUID_SETTINGS_SERVICE_ENCODE(3)
#define BT_UUID_SETTINGS_INFO_VAL BT_UUID_SETTINGS_SERVICE_ENCODE(4)

#define BT_UUID_SETTINGS_SERVICE  BT_UUID_DECLARE_128(BT_UUID_SETTINGS_SERVICE_VAL)
#define BT_UUID_SETTINGS_CONTROL  BT_UUID_DECLARE_128(BT_UUID_SETTINGS_CONTROL_VAL)
#define BT_UUID_SETTINGS_DATA     BT_UUID_DECLARE_128(BT_UUID_SETTINGS_DATA_VAL)
#define BT_UUID_SETTINGS_STATUS   BT_UUID_DECLARE_128(BT_UUID_SETTINGS_STATUS_VAL)
#define BT_UUID_SETTINGS_INFO     BT_UUID_DECLARE_128(BT_UUID_SETTINGS_INFO_VAL)

constexpr size_t MaxChunkSize = 512;
constexpr size_t MaxQueuedResults = 4;

struct ResultMessage {
    uint8_t settings_id;
    BleSettingsState state;
    BleSettingsErrorCode error_code;
};

K_MSGQ_DEFINE(ble_settings_result_msgq, sizeof(ResultMessage), MaxQueuedResults, 1);

bt_conn* BleSettingsService::ble_active_conn_{nullptr};
BleSettingsService::Callbacks BleSettingsService::callbacks_;
size_t BleSettingsService::max_transfer_size_{0};
k_mutex BleSettingsService::mutex_;
std::shared_ptr<BleSettingsStatus> BleSettingsService::status_{std::make_shared<BleSettingsStatus>()};
std::shared_ptr<std::pmr::vector<uint8_t>> BleSettingsService::transfer_buffer_;
BleSettingsCommandManager BleSettingsService::command_manager_{status_};
std::shared_ptr<WorkQueueThread> BleSettingsService::work_queue_thread_;
std::optional<WorkQueueTask<BleSettingsService::WorkTask>> BleSettingsService::transfer_task_;
std::optional<WorkQueueTask<BleSettingsService::WorkTask>> BleSettingsService::result_task_;
uint32_t BleSettingsService::transfer_generation_{0};

void BleSettingsService::Initialize(
    const Callbacks& callbacks,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    allocator_type allocator,
    size_t max_transfer_size) {

    k_mutex_init(&mutex_);

    k_mutex_lock(&mutex_, K_FOREVER);

    max_transfer_size_ = max_transfer_size;
    transfer_buffer_ = std::make_shared<std::pmr::vector<uint8_t>>(max_transfer_size_, allocator);
    transfer_buffer_->resize(max_transfer_size_);

    callbacks_ = callbacks;
    if(callbacks_.on_state_change)
        status_->SetStateChangeHandler(callbacks_.on_state_change);

    work_queue_thread_ = std::move(work_queue_thread);
    transfer_task_ = work_queue_thread_->CreateTask(TransferTaskHandler, static_cast<WorkTask*>(nullptr));
    result_task_ = work_queue_thread_->CreateTask(ResultTaskHandler, static_cast<WorkTask*>(nullptr));

    command_manager_.Initialize(
        transfer_buffer_, {
            .on_apply_requested = ScheduleTransfer,
            .on_read_requested = ScheduleTransfer,
            .on_result = QueueResult
        });

    status_->Reset();

    k_mutex_unlock(&mutex_);
}

void BleSettingsService::BleConnected(bt_conn* conn) {
    k_mutex_lock(&mutex_, K_FOREVER);
    if(ble_active_conn_)
        bt_conn_unref(ble_active_conn_);
    ble_active_conn_ = bt_conn_ref(conn);

    status_->Reset();
    k_msgq_purge(&ble_settings_result_msgq);
    k_mutex_unlock(&mutex_);
}

void BleSettingsService::BleDisconnected([[maybe_unused]] bt_conn* conn) {
    k_mutex_lock(&mutex_, K_FOREVER);
    if(ble_active_conn_)
        bt_conn_unref(ble_active_conn_);
    ble_active_conn_ = nullptr;

    // Leaving Reading or Applying also drops a transfer the work queue hasn't finished.
    status_->Reset();
    k_msgq_purge(&ble_settings_result_msgq);
    k_mutex_unlock(&mutex_);
}

void BleSettingsService::HandleDataChunk(std::span<const uint8_t> data) {
    if(status_->GetState() != BleSettingsState::Writing) {
        LOG_ERR("Data chunk received while not writing");
        return;
    }

    // Guard against overflow using the buffer's own size, not just the
    // client-supplied total_bytes, so the check remains valid if
    // max_transfer_size_ and the validation in StartWrite ever diverge.
    if(status_->GetTransferredBytes() + data.size() > status_->GetTotalBytes()
        || status_->GetTransferredBytes() + data.size() > transfer_buffer_->size()) {

        LOG_ERR("Data overflow: would exceed %u bytes", status_->GetTotalBytes());
        status_->SetErrorCode(BleSettingsErrorCode::DataOverflow);
        status_->SetState(BleSettingsState::Error);
        QueueResult(status_->GetSettingsId(), BleSettingsState::Error, BleSettingsErrorCode::DataOverflow);

        return;
    }

    std::ranges::copy(data, transfer_buffer_->begin() + status_->GetTransferredBytes());
    status_->SetTransferredBytes(status_->GetTransferredBytes() + data.size());

    LOG_DBG("Received chunk: %u bytes (total: %u/%u)",
        data.size(), status_->GetTransferredBytes(), status_->GetTotalBytes());
}

// Transfers and Result notifications
// ==================================

void BleSettingsService::ScheduleTransfer([[maybe_unused]] uint8_t settings_id) {
    // Runs under mutex_, right after EndWrite or RequestRead accepted a transfer.
    transfer_generation_++;
    transfer_task_->Schedule();
}

void BleSettingsService::QueueResult(uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code) {
    const ResultMessage result{settings_id, state, error_code};

    if(k_msgq_put(&ble_settings_result_msgq, &result, K_NO_WAIT) != 0) {
        LOG_WRN("Result queue full, dropping the result for settings_id=%u", settings_id);
        return;
    }

    result_task_->Schedule();
}

WorkQueueTaskResult BleSettingsService::TransferTaskHandler([[maybe_unused]] WorkTask* task) {
    k_mutex_lock(&mutex_, K_FOREVER);
    const BleSettingsState state = status_->GetState();
    const uint32_t generation = transfer_generation_;
    const uint8_t settings_id = status_->GetSettingsId();
    bt_conn* conn = state == BleSettingsState::Reading && ble_active_conn_ != nullptr
        ? bt_conn_ref(ble_active_conn_)
        : nullptr;
    k_mutex_unlock(&mutex_);

    if(state == BleSettingsState::Applying) {
        Apply();
    } else if(state == BleSettingsState::Reading) {
        if(conn == nullptr) {
            LOG_ERR("Read: no active connection");
            FailRead(generation, BleSettingsErrorCode::NotificationFailed);
            return {};
        }

        try {
            Read(conn, generation, settings_id);
        } catch(const std::exception& e) {
            LOG_ERR("Read threw: %s", e.what());
            FailRead(generation, BleSettingsErrorCode::HandlerFailed);
        } catch(...) {
            LOG_ERR("Read threw an unknown exception");
            FailRead(generation, BleSettingsErrorCode::HandlerFailed);
        }

        bt_conn_unref(conn);
    }

    return {};
}

void BleSettingsService::Apply() {
    // Held for the whole apply, so Data writes and a new upload can't touch the buffer meanwhile.
    k_mutex_lock(&mutex_, K_FOREVER);

    if(status_->GetState() != BleSettingsState::Applying) {
        k_mutex_unlock(&mutex_);
        return;
    }

    const uint8_t settings_id = status_->GetSettingsId();
    bool success = true;

    if(callbacks_.on_config_write) {
        try {
            success = callbacks_.on_config_write(
                settings_id, std::span(transfer_buffer_->data(), status_->GetTransferredBytes()));
        } catch(const std::exception& e) {
            LOG_ERR("Config write handler threw: %s", e.what());
            success = false;
        } catch(...) {
            LOG_ERR("Config write handler threw an unknown exception");
            success = false;
        }
    } else {
        LOG_WRN("No write handler registered");
    }

    if(success) {
        LOG_INF("Config write successful");
        status_->Reset();
        QueueResult(settings_id, BleSettingsState::Idle, BleSettingsErrorCode::None);
    } else {
        LOG_ERR("Config write handler failed");
        status_->SetErrorCode(BleSettingsErrorCode::HandlerFailed);
        status_->SetState(BleSettingsState::Error);
        QueueResult(settings_id, BleSettingsState::Error, BleSettingsErrorCode::HandlerFailed);
    }

    k_mutex_unlock(&mutex_);
}

bool BleSettingsService::IsCurrentRead(uint32_t generation) {
    return transfer_generation_ == generation && status_->GetState() == BleSettingsState::Reading;
}

void BleSettingsService::FailRead(uint32_t generation, BleSettingsErrorCode error_code) {
    k_mutex_lock(&mutex_, K_FOREVER);
    if(IsCurrentRead(generation)) {
        status_->SetErrorCode(error_code);
        status_->SetState(BleSettingsState::Error);
        QueueResult(status_->GetSettingsId(), BleSettingsState::Error, error_code);
    }
    k_mutex_unlock(&mutex_);
}

void BleSettingsService::Read(bt_conn* conn, uint32_t generation, uint8_t settings_id) {
    std::span<const uint8_t> data;
    if(callbacks_.on_config_read)
        data = callbacks_.on_config_read(settings_id);

    if(data.empty()) {
        LOG_ERR("Read: no configuration for settings_id=%u", settings_id);
        FailRead(generation, BleSettingsErrorCode::HandlerFailed);
        return;
    }

    if(!SendData(conn, generation, settings_id, data))
        FailRead(generation, BleSettingsErrorCode::NotificationFailed);
}

bool BleSettingsService::SendData(
    bt_conn* conn,
    uint32_t generation,
    uint8_t settings_id,
    std::span<const uint8_t> data) {

    const bt_gatt_attr* status_attr = bt_gatt_find_by_uuid(
        gatt_service_.attrs, (uint16_t)gatt_service_.attr_count, BT_UUID_SETTINGS_STATUS);
    const bt_gatt_attr* data_attr = bt_gatt_find_by_uuid(
        gatt_service_.attrs, (uint16_t)gatt_service_.attr_count, BT_UUID_SETTINGS_DATA);

    if(!status_attr || !data_attr) {
        LOG_ERR("SendData: BLE characteristics not found");
        return false;
    }

    const uint16_t mtu = bt_gatt_get_mtu(conn);
    if(mtu <= 3) {
        LOG_ERR("SendData: invalid MTU %u", mtu);
        return false;
    }

    const size_t chunk_size = std::min<size_t>(mtu - 3, MaxChunkSize);

    LOG_INF("SendData: Sending config: id=%u, size=%zu, chunk=%zu", settings_id, data.size(), chunk_size);

    // Off the BT RX thread, a notification waits for a free buffer, which paces the transfer.
    auto start_msg = BleSettingsCommandStartRead::Create(settings_id, data.size());
    int err = bt_gatt_notify(conn, status_attr, start_msg.data(), (uint16_t)start_msg.size());
    if(err) {
        LOG_ERR("SendData: Failed to send StartRead notification (err %d)", err);
        return false;
    }

    for(size_t offset = 0; offset < data.size();) {
        const size_t length = std::min(chunk_size, data.size() - offset);

        err = bt_gatt_notify(conn, data_attr, data.data() + offset, (uint16_t)length);
        if(err) {
            LOG_ERR("SendData: Notification failed at offset %zu (err %d)", offset, err);
            return false;
        }

        offset += length;

        k_mutex_lock(&mutex_, K_FOREVER);
        const bool current = IsCurrentRead(generation);
        k_mutex_unlock(&mutex_);

        // Abort, a disconnect or another command ended this download.
        if(!current) {
            LOG_INF("SendData: stopped after %zu/%zu bytes", offset, data.size());
            return false;
        }
    }

    auto end_msg = BleSettingsCommandEndRead::Create();
    err = bt_gatt_notify(conn, status_attr, end_msg.data(), (uint16_t)end_msg.size());
    if(err) {
        LOG_ERR("SendData: Failed to send EndRead notification (err %d)", err);
        return false;
    }

    k_mutex_lock(&mutex_, K_FOREVER);
    if(IsCurrentRead(generation))
        status_->Reset();
    k_mutex_unlock(&mutex_);

    LOG_INF("SendData: Config sent successfully");

    return true;
}

WorkQueueTaskResult BleSettingsService::ResultTaskHandler([[maybe_unused]] WorkTask* task) {
    const bt_gatt_attr* status_attr = bt_gatt_find_by_uuid(
        gatt_service_.attrs, (uint16_t)gatt_service_.attr_count, BT_UUID_SETTINGS_STATUS);

    k_mutex_lock(&mutex_, K_FOREVER);
    bt_conn* conn = ble_active_conn_ != nullptr ? bt_conn_ref(ble_active_conn_) : nullptr;
    k_mutex_unlock(&mutex_);

    ResultMessage result;
    while(k_msgq_get(&ble_settings_result_msgq, &result, K_NO_WAIT) == 0) {
        if(conn == nullptr || status_attr == nullptr)
            continue;

        try {
            auto message = BleSettingsCommandResult::Create(result.settings_id, result.state, result.error_code);
            int err = bt_gatt_notify(conn, status_attr, message.data(), (uint16_t)message.size());
            if(err)
                LOG_WRN("Failed to send the Result notification (err %d)", err);
        } catch(const std::exception& e) {
            LOG_ERR("Sending the Result notification threw: %s", e.what());
        }
    }

    if(conn != nullptr)
        bt_conn_unref(conn);

    return {};
}

// GATT callbacks
// ==============

ssize_t ControlWriteCallback(
    [[maybe_unused]] bt_conn* conn,
    [[maybe_unused]] const bt_gatt_attr* attr,
    const void* buf,
    uint16_t len,
    uint16_t offset,
    [[maybe_unused]] uint8_t flags) {

    if(offset != 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

    k_mutex_lock(&BleSettingsService::mutex_, K_FOREVER);
    try {
        BleSettingsService::command_manager_.Process(
            std::span(static_cast<const uint8_t*>(buf), len));
    } catch(const std::exception& e) {
        LOG_ERR("Control command processing threw: %s", e.what());
    } catch(...) {
        LOG_ERR("Control command processing threw an unknown exception");
    }
    k_mutex_unlock(&BleSettingsService::mutex_);

    return len;
}

ssize_t DataWriteCallback(
    [[maybe_unused]] bt_conn* conn,
    [[maybe_unused]] const bt_gatt_attr* attr,
    const void* buf,
    uint16_t len,
    uint16_t offset,
    [[maybe_unused]] uint8_t flags) {

    if(offset != 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

    k_mutex_lock(&BleSettingsService::mutex_, K_FOREVER);
    try {
        BleSettingsService::HandleDataChunk(
            std::span(static_cast<const uint8_t*>(buf), len));
    } catch(const std::exception& e) {
        LOG_ERR("Data chunk handling threw: %s", e.what());
    } catch(...) {
        LOG_ERR("Data chunk handling threw an unknown exception");
    }
    k_mutex_unlock(&BleSettingsService::mutex_);

    return len;
}

// NOTE: Data format:
//       [0] - protocol version
//       [1-4] - maximum transfer size, uint32_t (little-endian)
static ssize_t InfoReadCallback(
    bt_conn* conn,
    const bt_gatt_attr* attr,
    void* buf,
    uint16_t len,
    uint16_t offset) {

    uint8_t info[5] = { BleSettingsService::ProtocolVersion };
    sys_put_le32(static_cast<uint32_t>(BleSettingsService::GetMaxTransferSize()), &info[1]);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

// GATT Service Definition
// =======================

// NOTE: This is supposed to be defined with BT_GATT_SERVICE_DEFINE(gatt_service_, ...) macro,
//       but in order to make gatt_service_ a class member macro has been expanded manually.
static const bt_gatt_attr gatt_attributes_[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_SETTINGS_SERVICE),

    // Changing settings needs an encrypted link; the unit asks for one when a central connects.
    BT_GATT_CHARACTERISTIC(BT_UUID_SETTINGS_CONTROL,
                        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                        BT_GATT_PERM_WRITE_ENCRYPT,
                        nullptr, &ControlWriteCallback, nullptr),

    BT_GATT_CHARACTERISTIC(BT_UUID_SETTINGS_DATA,
                        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
                        BT_GATT_PERM_WRITE_ENCRYPT,
                        nullptr, &DataWriteCallback, nullptr),
    // Centrals subscribe while connecting, possibly before pairing is done, so subscribing stays open.
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Every notification answers an encrypted command, so the value needs no permission of its own.
    BT_GATT_CHARACTERISTIC(BT_UUID_SETTINGS_STATUS,
                        BT_GATT_CHRC_NOTIFY,
                        BT_GATT_PERM_NONE,
                        nullptr, nullptr, nullptr),
    BT_GATT_CCC(nullptr, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // Readable without pairing, so a client can check the protocol version first.
    BT_GATT_CHARACTERISTIC(BT_UUID_SETTINGS_INFO,
                        BT_GATT_CHRC_READ,
                        BT_GATT_PERM_READ,
                        &InfoReadCallback, nullptr, nullptr),
};

const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, BleSettingsService::gatt_service_) =
    BT_GATT_SERVICE(gatt_attributes_);

} // namespace eerie_leap::subsys::bluetooth::ble_settings
