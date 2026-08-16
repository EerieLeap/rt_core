#include <zephyr/logging/log.h>

#include "subsys/cdmp/utilities/cdmp_status_machine.h"
#include "subsys/cdmp/utilities/cdmp_helpers.h"

#include "cdmp_network_service.h"

LOG_MODULE_REGISTER(cdmp_network_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::utilities;
using namespace eerie_leap::subsys::cdmp::models;

CdmpNetworkService::CdmpNetworkService(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device,
    std::shared_ptr<WorkQueueThread> work_queue_thread)
        : CdmpCanbusServiceBase(std::move(can_id_manager), std::move(device)),
        work_queue_thread_(std::move(work_queue_thread)) {}

CdmpNetworkService::~CdmpNetworkService() {
    Stop();
}

bool CdmpNetworkService::DoInitialize() {
    validation_task_ = work_queue_thread_->CreateTask(
        ProcessPeriodicValidation, this);

    return true;
}

bool CdmpNetworkService::DoStart() {
    RegisterCanHandlers();
    StartValidationTask();

    LOG_INF("CDMP Network Service started");

    return true;
}

bool CdmpNetworkService::DoStop() {
    is_validation_task_running_ = false;
    if(validation_task_.has_value())
        validation_task_.value().Cancel();

    ClearAllDevices();
    UnregisterCanHandlers();

    LOG_INF("CDMP Network Service stopped");

    return true;
}

void CdmpNetworkService::StartValidationTask() {
    if(!is_validation_task_running_ && device_->GetStatus() == CdmpDeviceStatus::ONLINE) {
        validation_task_.value().Schedule();
        is_validation_task_running_ = true;
    }
}

void CdmpNetworkService::RegisterCanHandlers() {
    if (!canbus_ || !canbus_->IsValid()) {
        LOG_ERR("Cannot register CAN handlers: CAN bus is not valid");
        return;
    }

    canbus_handler_id_ = (*canbus_)->RegisterFrameReceivedHandler(
        can_id_manager_->GetManagementCanId(),
        [this](const CanFrame& frame) {
            work_queue_thread_->Run([this, frame]() { ProcessFrame(frame.data); }); });

    if(canbus_handler_id_ < 0) {
        throw std::runtime_error("Failed to register CAN frame handler for frame ID: "
            + std::to_string(can_id_manager_->GetManagementCanId()));
    }
}

void CdmpNetworkService::UnregisterCanHandlers() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    if(canbus_handler_id_ >= 0)
        (*canbus_)->RemoveFrameReceivedHandler(can_id_manager_->GetManagementCanId(), canbus_handler_id_);
    canbus_handler_id_ = -1;
}

void CdmpNetworkService::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    switch(new_status) {
        case CdmpDeviceStatus::INIT:
            work_queue_thread_->Run([this]() { StartInitialization(); });
            break;

        case CdmpDeviceStatus::CLAIMING:
            work_queue_thread_->Run([this]() { SendIdClaim(); });
            break;

        case CdmpDeviceStatus::ONLINE:
            LOG_INF("Device %d is now online", device_->GetDeviceId());
            StartValidationTask();
            break;

        case CdmpDeviceStatus::VERSION_MISMATCH:
            LOG_ERR("Protocol version mismatch detected");
            Stop();
            break;

        case CdmpDeviceStatus::ERROR:
            LOG_ERR("Device entered error state");
            Stop();
            break;

        default:
            break;
    }
}

void CdmpNetworkService::ProcessFrame(std::span<const uint8_t> frame_data) {
    CdmpManagementMessageType message_type = static_cast<CdmpManagementMessageType>(frame_data[0]);

    switch(message_type) {
        case CdmpManagementMessageType::DISCOVERY_REQUEST:
            ProcessDiscoveryRequestFrame(frame_data);
            break;

        case CdmpManagementMessageType::DISCOVERY_RESPONSE:
            ProcessDiscoveryResponseFrame(frame_data);
            break;

        case CdmpManagementMessageType::ID_CLAIM:
            ProcessIdClaimRequestFrame(frame_data);
            break;

        case CdmpManagementMessageType::ID_CLAIM_RESPONSE:
            ProcessIdClaimResponseFrame(frame_data);
            break;
    }
}

void CdmpNetworkService::StartInitialization() {
    discovery_response_received_ = false;
    int discovery_backoff_resets = 0;

    int backoff_index = 0;
    for(backoff_index = 0; backoff_index < CdmpTimeouts::RETRY_BACKOFFS_MS.size(); ++backoff_index) {
        if(device_->GetStatus() != CdmpDeviceStatus::INIT)
            return;

        discovery_requesting_uids_.clear();
        const auto& backoff = CdmpTimeouts::RETRY_BACKOFFS_MS[backoff_index];

        SendDiscoveryRequest();
        LOG_INF("Sending discovery request with backoff: %d ms", backoff);
        k_msleep(backoff);

        if(discovery_response_received_)
            break;

        if(!discovery_requesting_uids_.empty()) {
            for(const auto& uid : discovery_requesting_uids_) {
                if(device_->GetUniqueIdentifier() > uid && discovery_backoff_resets < CdmpConstants::DISCOVERY_MAX_BACKOFF_RESETS) {
                    discovery_backoff_resets++;
                    backoff_index = 0;
                }
            }
        }
    }

    device_->ClaimId();
}

void CdmpNetworkService::ProcessDiscoveryRequestFrame(std::span<const uint8_t> frame_data) {
    LOG_INF("Processing discovery request frame");

    try {
        auto message = CdmpDiscoveryRequestMessage::FromCanFrame(frame_data);

        if(message.uid == 0 || message.uid == device_->GetUniqueIdentifier()) {
            LOG_ERR("Device UID conflict detected during discovery - received UID: %08X",
                message.uid);
        }

        discovery_requesting_uids_.push_back(message.uid);
        work_queue_thread_->Run([this]() { SendDiscoveryResponse(); });
    } catch (const std::exception& e) {
        LOG_ERR("Error processing discovery request frame: %s", e.what());
    }
}

void CdmpNetworkService::ProcessDiscoveryResponseFrame(std::span<const uint8_t> frame_data) {
    LOG_INF("Processing discovery response frame");

    try {
        auto message = CdmpDiscoveryResponseMessage::FromCanFrame(frame_data);
        discovery_response_received_ = true;

        if(message.uid == device_->GetUniqueIdentifier()) {
            LOG_ERR("Device UID conflict detected during discovery - received UID: %08X",
                message.uid);
            device_->EnterError();

            return;
        }

        UpdateDeviceFromDiscovery(message);
    } catch (const std::exception& e) {
        LOG_ERR("Error processing discovery response frame: %s", e.what());
    }
}

void CdmpNetworkService::SendDiscoveryRequest() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    CdmpDiscoveryRequestMessage message{};
    message.uid = device_->GetUniqueIdentifier();
    auto frame_data = message.ToCanFrame();
    uint32_t frame_id = can_id_manager_->GetDiscoveryRequestCanId();
    (*canbus_)->SendFrame(frame_id, frame_data);
}

void CdmpNetworkService::SendDiscoveryResponse() {
    if(!device_->IsOnline())
        return;

    if(!canbus_ || !canbus_->IsValid())
        return;

    CdmpDiscoveryResponseMessage message = {};
    message.device_id = device_->GetDeviceId();
    message.uid = device_->GetUniqueIdentifier();
    message.device_type = device_->GetDeviceType();

    k_msleep(device_->GetStaggeredMessageDelay());

    auto frame_data = message.ToCanFrame();
    uint32_t frame_id = can_id_manager_->GetDiscoveryResponseCanId();
    (*canbus_)->SendFrame(frame_id, frame_data);

    LOG_INF("Sent discovery response for device %d", message.device_id);
}

void CdmpNetworkService::SendIdClaim() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    try {
        claiming_device_id_ = GetLowestAvailableId(claiming_device_id_);
    } catch (const std::exception& e) {
        LOG_ERR("Error getting lowest available ID: %s", e.what());
        device_->EnterError();
        claiming_device_id_ = 0;

        return;
    }

    for(int i = 0; i < CdmpConstants::ID_CLAIM_MAX_ATTEMPTS; ++i) {
        CdmpIdClaimRequestMessage message = {};
        message.claiming_device_id = claiming_device_id_;
        message.uid = device_->GetUniqueIdentifier();
        message.device_type = device_->GetDeviceType();
        message.protocol_version = device_->GetProtocolVersion();

        auto frame_data = message.ToCanFrame();
        uint32_t frame_id = can_id_manager_->GetIdClaimRequestCanId();
        (*canbus_)->SendFrame(frame_id, frame_data);
        LOG_INF("Sent ID claim for device %d", message.claiming_device_id);

        k_msleep(CdmpConstants::ID_CLAIM_RESPONSE_TIMEOUT_MS);

        if(id_claim_result_.has_value() && id_claim_result_.value() == CdmpIdClaimResult::VERSION_INCOMPATIBLE) {
            device_->EnterVersionMismatch();
            claiming_device_id_ = 0;
            return;
        }

        if(id_claim_result_.has_value() && id_claim_result_.value() == CdmpIdClaimResult::ACCEPT) {
            device_->SetDeviceId(claiming_device_id_);
            UpdateNetworkDevices();
            device_->GoOnline();
            break;
        }
    }

    // First or only device on the network, assign the ID
    if(!id_claim_result_.has_value()) {
        device_->SetDeviceId(claiming_device_id_);
        device_->GoOnline();
    } else if(id_claim_result_.has_value() && id_claim_result_.value() == CdmpIdClaimResult::REJECT) {
        LOG_ERR("All ID claims were rejected.");
        device_->EnterError();
    }

    claiming_device_id_ = 0;
}

void CdmpNetworkService::ProcessIdClaimRequestFrame(std::span<const uint8_t> frame_data) {
    if(!canbus_ || !canbus_->IsValid())
        return;

    try {
        if(!device_->IsOnline())
            return;

        CdmpIdClaimRequestMessage id_claim_message = CdmpIdClaimRequestMessage::FromCanFrame(frame_data);
        LOG_INF("Received ID claim from device %d", id_claim_message.claiming_device_id);

        CdmpIdClaimResponseMessage message = {};
        message.responding_device_id = device_->GetDeviceId();
        message.claiming_device_id = id_claim_message.claiming_device_id;
        message.claiming_device_uid = id_claim_message.uid;

        bool send_response = false;

        if(device_->IsLowestIdOnNetwork()
           && !device_->IsProtocolCompatible(id_claim_message.protocol_version)) {

            message.result = CdmpIdClaimResult::VERSION_INCOMPATIBLE;
            send_response = true;
        } else if(device_->GetDeviceId() == id_claim_message.claiming_device_id) {
            message.result = CdmpIdClaimResult::REJECT;
            send_response = true;
        } else if(device_->IsLowestIdOnNetwork()) {
            message.result = CdmpIdClaimResult::ACCEPT;
            send_response = true;

            AddOrUpdateDevice(
                id_claim_message.claiming_device_id,
                id_claim_message.device_type,
                id_claim_message.uid);
        }

        if(send_response) {
            auto frame_data = message.ToCanFrame();
            uint32_t response_id = can_id_manager_->GetIdClaimResponseCanId();
            (*canbus_)->SendFrame(response_id, frame_data);

            return;
        }
    } catch (const std::exception& e) {
        LOG_ERR("Error processing ID claim frame: %s", e.what());
    }
}

void CdmpNetworkService::ProcessIdClaimResponseFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpIdClaimResponseMessage response = CdmpIdClaimResponseMessage::FromCanFrame(frame_data);

        if(device_->IsOnline())
            return;

        if(response.claiming_device_id != claiming_device_id_)
            return;

        LOG_INF("Received ID claim response from device %d for device %d with result %d",
            response.responding_device_id, response.claiming_device_id, static_cast<int>(response.result));

        id_claim_result_ = response.result;
    } catch (const std::exception& e) {
        LOG_ERR("Error processing ID claim response frame: %s", e.what());
    }
}

void CdmpNetworkService::UpdateDeviceFromDiscovery(const CdmpDiscoveryResponseMessage& discovery) {
    AddOrUpdateDevice(discovery.device_id, discovery.device_type, discovery.uid);
}

void CdmpNetworkService::UpdateDeviceFromHeartbeat(const CdmpHeartbeatMessage& heartbeat) {
    uint8_t device_id = heartbeat.device_id;

    if(!network_devices_.contains(device_id)) {
        LOG_DBG("Heartbeat from unknown device %d", device_id);
        return;
    }

    AddOrUpdateDevice(
        heartbeat.device_id,
        network_devices_.at(device_id)->GetDeviceType(),
        network_devices_.at(device_id)->GetUniqueIdentifier(),
        heartbeat.capability_flags);
}

void CdmpNetworkService::AddOrUpdateDevice(
    uint8_t device_id,
    CdmpDeviceType device_type,
    uint32_t uid,
    uint32_t capability_flags) {

    if(network_devices_.contains(device_id)) {
        auto* device = network_devices_.at(device_id).get();

        device->UpdateHeartbeat();
        device->GoOnline(true);
        if(capability_flags != 0)
            device->SetCapabilityFlags(capability_flags);

        LOG_DBG("Updated device %d.", device_id);
    } else {
        auto device = std::make_unique<CdmpDevice>(uid, device_type, CdmpDeviceStatus::ONLINE);
        device->SetDeviceId(device_id);
        if(capability_flags != 0)
            device->SetCapabilityFlags(capability_flags);

        network_devices_.emplace(device_id, std::move(device));
        UpdateNetworkDevices();

        LOG_INF("Added new device: ID=%d, Type=%d, UID=0x%08X", device_id, std::to_underlying(device_type), uid);
    }
}

void CdmpNetworkService::RemoveDevice(uint8_t device_id) {
    if(network_devices_.contains(device_id)) {
        network_devices_.erase(device_id);
        LOG_INF("Removed device %d", device_id);
    }
}

void CdmpNetworkService::ClearAllDevices() {
    network_devices_.clear();
    LOG_INF("Cleared all network devices");
}

std::vector<uint8_t> CdmpNetworkService::GetOnlineDeviceIds() const {
    std::vector<uint8_t> online_devices;
    for(const auto& [device_id, device] : network_devices_) {
        if(device->GetStatus() == CdmpDeviceStatus::ONLINE)
            online_devices.push_back(device_id);
    }
    return online_devices;
}

std::vector<uint8_t> CdmpNetworkService::GetAllDeviceIds() const {
    std::vector<uint8_t> all_devices;
    for(const auto& [device_id, device] : network_devices_)
        all_devices.push_back(device_id);

    return all_devices;
}

const CdmpDevice* CdmpNetworkService::GetDevice(uint8_t device_id) const {
    if(!network_devices_.contains(device_id))
        return nullptr;

    return network_devices_.at(device_id).get();
}

CdmpDevice* CdmpNetworkService::GetDevice(uint8_t device_id) {
    if(!network_devices_.contains(device_id))
        return nullptr;

    return network_devices_.at(device_id).get();
}

void CdmpNetworkService::SetAutoDiscovery(bool enabled) {
    auto_discovery_enabled_ = enabled;
    LOG_INF("Auto discovery %s", enabled ? "enabled" : "disabled");
}

WorkQueueTaskResult CdmpNetworkService::ProcessPeriodicValidation(CdmpNetworkService* instance) {
    if(!instance->is_validation_task_running_) {
        return {
            .reschedule = false
        };
    }

    instance->RemoveOfflineDevices();
    instance->UpdateNetworkDevices();

    return {
        .reschedule = instance->is_validation_task_running_,
        .delay = K_MSEC(CdmpConstants::NETWORK_VALIDATION_INTERVAL_MS)
    };
}

bool CdmpNetworkService::IsDeviceOnline(uint8_t device_id) const {
    const auto* device = GetDevice(device_id);
    return device && device->GetStatus() == CdmpDeviceStatus::ONLINE;
}

void CdmpNetworkService::RemoveOfflineDevices() {
    std::vector<uint8_t> offline_devices;
    for(const auto& [device_id, device] : network_devices_) {
        if(device->GetLastHeartbeatDeltaMs() > CdmpConstants::HEARTBEAT_TIMEOUT_MS)
            offline_devices.push_back(device_id);
    }

    for(auto device_id : offline_devices)
        RemoveDevice(device_id);
}

void CdmpNetworkService::UpdateNetworkDevices() {
    UpdateLowestIdOnNetwork();
    UpdateStaggeredMessageDelay();
}

void CdmpNetworkService::UpdateLowestIdOnNetwork() {
    uint8_t lowest_id_on_network = device_->GetDeviceId();
    for(const auto& [id, _] : network_devices_) {
        if(id < lowest_id_on_network)
            lowest_id_on_network = std::min(lowest_id_on_network, id);
    }

    device_->SetIsLowestIdOnNetwork(device_->GetDeviceId() == lowest_id_on_network);
    for(const auto& [id, device] : network_devices_)
        device->SetIsLowestIdOnNetwork(id == lowest_id_on_network);
}

void CdmpNetworkService::UpdateStaggeredMessageDelay() {
    uint8_t lowest_id_on_network = device_->GetDeviceId();
    for(const auto& [id, _] : network_devices_) {
        if(id < lowest_id_on_network)
            lowest_id_on_network = std::min(lowest_id_on_network, id);
    }

    device_->SetStaggeredMessageDelay(CdmpHelpers::CalculateStaggeredMessageTimeOffset(
        GetAllDeviceIds(), device_->GetDeviceId()));
    for(const auto& [id, device] : network_devices_)
        device->SetStaggeredMessageDelay(CdmpHelpers::CalculateStaggeredMessageTimeOffset(
            GetAllDeviceIds(), id));
}

uint8_t CdmpNetworkService::GetLowestAvailableId(uint8_t after) const {
    for(uint8_t i = after + 1; i < 255; ++i) {
        if(!network_devices_.contains(i))
            return i;
    }

    throw std::runtime_error("No available device ID found");
}

void CdmpNetworkService::PrintNetworkStatus() const {
    LOG_INF("Network Status: %zu devices total", network_devices_.size());

    for(const auto& [device_id, device] : network_devices_) {
        LOG_INF("  Device %d: Type=%d, Status=%d, UID=0x%08X",
            device_id, std::to_underlying(device->GetDeviceType()),
            std::to_underlying(device->GetStatus()), device->GetUniqueIdentifier());
    }
}

} // namespace eerie_leap::subsys::cdmp::services
