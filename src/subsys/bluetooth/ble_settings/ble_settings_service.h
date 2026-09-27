#pragma once

#include <span>
#include <functional>
#include <cstdint>
#include <optional>
#include <vector>
#include <memory_resource>
#include <memory>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_thread.h"

#include "ble_settings_command/ble_settings_command_manager.h"
#include "ble_settings_status.h"

namespace eerie_leap::subsys::bluetooth::ble_settings {

using eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command::BleSettingsCommandManager;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

// Base UUID: e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f40000
#define BT_UUID_SETTINGS_SERVICE_ENCODE(characteristic_id) \
    BT_UUID_128_ENCODE(0xe7a1b2c3, 0xd4e5, 0x6f78, 0x9a0b, 0xc1d2e3f40000 + (characteristic_id))

#define BT_UUID_SETTINGS_SERVICE_VAL BT_UUID_SETTINGS_SERVICE_ENCODE(0)

class BleSettingsService {
public:
    using allocator_type = std::pmr::polymorphic_allocator<>;
    using WriteHandler = std::function<bool(uint8_t settings_id, std::span<const uint8_t> data)>;
    using ReadHandler = std::function<std::span<const uint8_t>(uint8_t settings_id)>;

    struct Callbacks {
        WriteHandler on_config_write;
        ReadHandler on_config_read;
        BleSettingsStatus::StateChangeHandler on_state_change;
    };

    static constexpr uint8_t ProtocolVersion = 1;
    static constexpr size_t DefaultMaxTransferSize = 64 * 1024;

private:
    struct WorkTask {};

    static bt_conn* ble_active_conn_;
    static size_t max_transfer_size_;
    static const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, gatt_service_);

    static Callbacks callbacks_;
    static k_mutex mutex_;
    static std::shared_ptr<BleSettingsStatus> status_;
    static std::shared_ptr<std::pmr::vector<uint8_t>> transfer_buffer_;
    static BleSettingsCommandManager command_manager_;

    // Accepted transfers and Result notifications run here, so GATT callbacks never wait for them.
    static std::shared_ptr<WorkQueueThread> work_queue_thread_;
    static std::optional<WorkQueueTask<WorkTask>> transfer_task_;
    static std::optional<WorkQueueTask<WorkTask>> result_task_;
    // Tells a download that a newer transfer replaced it. Guarded by mutex_.
    static uint32_t transfer_generation_;

    BleSettingsService() = default;
    ~BleSettingsService() = default;

    BleSettingsService(const BleSettingsService&) = delete;
    BleSettingsService& operator=(const BleSettingsService&) = delete;

    static void SetState(BleSettingsState new_state);
    static void HandleDataChunk(std::span<const uint8_t> data);

    static void ScheduleTransfer(uint8_t settings_id);
    static void QueueResult(uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code);
    static WorkQueueTaskResult TransferTaskHandler(WorkTask* task);
    static WorkQueueTaskResult ResultTaskHandler(WorkTask* task);
    static void Apply();
    static void Read(bt_conn* conn, uint32_t generation, uint8_t settings_id);
    static bool SendData(bt_conn* conn, uint32_t generation, uint8_t settings_id, std::span<const uint8_t> data);
    // Requires mutex_.
    static bool IsCurrentRead(uint32_t generation);
    static void FailRead(uint32_t generation, BleSettingsErrorCode error_code);

    friend ssize_t ControlWriteCallback(
        bt_conn* conn,
        const bt_gatt_attr* attr,
        const void* buf,
        uint16_t len,
        uint16_t offset,
        uint8_t flags);

    friend ssize_t DataWriteCallback(
        bt_conn* conn,
        const bt_gatt_attr* attr,
        const void* buf,
        uint16_t len,
        uint16_t offset,
        uint8_t flags);

public:
    static void Initialize(
        const Callbacks& callbacks,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        allocator_type allocator = std::pmr::get_default_resource(),
        size_t max_transfer_size = DefaultMaxTransferSize);

    static void BleConnected(bt_conn* conn);
    static void BleDisconnected(bt_conn* conn);

    [[nodiscard]] static size_t GetMaxTransferSize() { return max_transfer_size_; }
};

} // namespace eerie_leap::subsys::bluetooth::ble_settings
