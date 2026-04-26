#include <zephyr/logging/log.h>

#include "cdmp_heartbeat_service.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"

LOG_MODULE_REGISTER(cdmp_heartbeat_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

CdmpHeartbeatService::CdmpHeartbeatService(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CdmpNetworkService> network_service)
    : CdmpCanbusServiceBase(std::move(can_id_manager), std::move(device)),
    work_queue_thread_(std::move(work_queue_thread)),
    network_service_(std::move(network_service)) {}

CdmpHeartbeatService::~CdmpHeartbeatService() {
    Stop();
}

void CdmpHeartbeatService::Initialize() {
    heartbeat_task_ = work_queue_thread_->CreateTask(
        ProcessPeriodicHeartbeat, this);
}

void CdmpHeartbeatService::Start() {
    if(atomic_get(&is_running_) != 0)
        return;

    atomic_set(&is_running_, 1);
    RegisterCanHandlers();
    StartHeartbeatTask();
}

void CdmpHeartbeatService::Stop() {
    if(atomic_get(&is_running_) == 0)
        return;

    atomic_set(&is_running_, 0);

    is_heartbeat_task_running_ = false;
    if(heartbeat_task_.has_value())
        heartbeat_task_.value().Cancel();

    UnregisterCanHandlers();

    LOG_INF("CDMP Heartbeat Service stopped");
}

void CdmpHeartbeatService::StartHeartbeatTask() {
    if(heartbeat_enabled_ && !is_heartbeat_task_running_ && device_->GetStatus() == CdmpDeviceStatus::ONLINE) {
        heartbeat_task_.value().Schedule();
        is_heartbeat_task_running_ = true;

        LOG_INF("CDMP Heartbeat Service started, interval: %d ms", CdmpConstants::HEARTBEAT_INTERVAL_MS);
    }
}

void CdmpHeartbeatService::RegisterCanHandlers() {
    canbus_handler_id_ = canbus_->RegisterFrameReceivedHandler(
        can_id_manager_->GetHeartbeatCanId(),
        [this](const CanFrame& frame) {
            work_queue_thread_->Run([this, frame]() { ProcessFrame(frame.data); }); });

    if(canbus_handler_id_ < 0) {
        throw std::runtime_error("Failed to register CAN frame handler for frame ID: "
            + std::to_string(can_id_manager_->GetHeartbeatCanId()));
    }
}

void CdmpHeartbeatService::UnregisterCanHandlers() {
    if(!canbus_)
        return;

    if(canbus_handler_id_ >= 0)
        canbus_->RemoveFrameReceivedHandler(can_id_manager_->GetHeartbeatCanId(), canbus_handler_id_);
    canbus_handler_id_ = -1;
}

void CdmpHeartbeatService::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    switch(new_status) {
        case CdmpDeviceStatus::ONLINE:
            LOG_INF("Device %d starting to send heartbeats", device_->GetDeviceId());
            StartHeartbeatTask();
            break;

        case CdmpDeviceStatus::OFFLINE:
        case CdmpDeviceStatus::VERSION_MISMATCH:
        case CdmpDeviceStatus::ERROR:
            Stop();
            break;

        default:
            break;
    }
}

void CdmpHeartbeatService::ProcessFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpHeartbeatMessage heartbeat = CdmpHeartbeatMessage::FromCanFrame(frame_data);
        network_service_->UpdateDeviceFromHeartbeat(heartbeat);
        LOG_DBG("Processed heartbeat frame");
    } catch (const std::exception& e) {
        LOG_ERR("Error processing heartbeat frame: %s", e.what());
    }
}

void CdmpHeartbeatService::SendHeartbeat() {
    if(!canbus_ || !heartbeat_enabled_)
        return;

    try {
        CdmpHeartbeatMessage heartbeat{
            .device_id = device_->GetDeviceId(),
            .health_status = device_->GetHealthStatus(),
            .sequence_number = heartbeat_sequence_number_,
            .capability_flags = device_->GetCapabilityFlags()
        };

        auto frame_data = heartbeat.ToCanFrame();
        uint32_t frame_id = can_id_manager_->GetHeartbeatCanId();
        canbus_->SendFrame(frame_id, frame_data);

        heartbeat_sequence_number_++;

        LOG_DBG("Sent heartbeat, sequence: %d", heartbeat_sequence_number_ - 1);
    } catch (const std::exception& e) {
        LOG_ERR("Error sending heartbeat: %s", e.what());
    }
}

void CdmpHeartbeatService::SetHeartbeatEnabled(bool enabled) {
    heartbeat_enabled_ = enabled;
    LOG_INF("Heartbeat %s", enabled ? "enabled" : "disabled");
}

WorkQueueTaskResult CdmpHeartbeatService::ProcessPeriodicHeartbeat(CdmpHeartbeatService* instance) {
    if(!instance->heartbeat_enabled_) {
        return {
            .reschedule = false
        };
    }

    instance->SendHeartbeat();

    return {
        .reschedule = instance->heartbeat_enabled_,
        .delay = K_MSEC(CdmpConstants::HEARTBEAT_INTERVAL_MS
            + instance->device_->GetStaggeredMessageDelay())
    };
}

void CdmpHeartbeatService::PrintHeartbeatStatus() const {
    LOG_INF("Heartbeat Status:");
    LOG_INF("  Enabled: %s", heartbeat_enabled_ ? "Yes" : "No");
    LOG_INF("  Interval: %d ms", CdmpConstants::HEARTBEAT_INTERVAL_MS);
}

} // namespace eerie_leap::subsys::cdmp::services
