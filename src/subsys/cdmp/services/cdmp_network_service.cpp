#include <algorithm>

#include <zephyr/logging/log.h>

#include "subsys/cdmp/utilities/cdmp_helpers.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"
#include "subsys/threading/scoped_mutex.h"

#include "cdmp_network_service.h"

LOG_MODULE_REGISTER(cdmp_network_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::utilities;
using namespace eerie_leap::subsys::cdmp::models;

using eerie_leap::subsys::threading::ScopedMutex;

namespace {

CdmpDeviceInfo ToDeviceInfo(const CdmpDevice& device) {
    return { .device_id = device.GetDeviceId(),
        .uid = device.GetUniqueIdentifier(),
        .features = device.GetFeatures(),
        .status = device.GetStatus() };
}

} // namespace

CdmpNetworkService::CdmpNetworkService(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device,
    std::shared_ptr<WorkQueueThread> work_queue_thread
) :
    CdmpCanbusServiceBase(std::move(can_id_manager), std::move(device)),
    work_queue_thread_(std::move(work_queue_thread)) {

    k_mutex_init(&devices_lock_);
    k_mutex_init(&handlers_lock_);
}

CdmpNetworkService::~CdmpNetworkService() {
    Stop();
}

bool CdmpNetworkService::DoInitialize() {
    validation_task_ = work_queue_thread_->CreateTask(ProcessPeriodicValidation, this);
    discovery_task_ = work_queue_thread_->CreateTask(ProcessDiscoveryAttempt, this);
    id_claim_task_ = work_queue_thread_->CreateTask(ProcessIdClaimAttempt, this);

    return true;
}

bool CdmpNetworkService::DoStart() {
    // Every session starts from an empty table, so a JOINED event is never lost to a stale entry.
    ClearAllDevices();
    RegisterCanHandlers();
    StartValidationTask();

    LOG_INF("CDMP Network Service started");

    return true;
}

bool CdmpNetworkService::DoStop() {
    UnregisterCanHandlers();
    // No frame handler runs once it is unregistered, so this drops everything it queued.
    atomic_inc(&session_);

    // Before the validation task, because a claim that completes meanwhile schedules it.
    if(discovery_task_.has_value())
        discovery_task_.value().Cancel();
    if(id_claim_task_.has_value())
        id_claim_task_.value().Cancel();

    is_validation_task_running_ = false;
    if(validation_task_.has_value())
        validation_task_.value().Cancel();

    ClearAllDevices();

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
    if(!canbus_ || !canbus_->IsValid()) {
        LOG_ERR("Cannot register CAN handlers: CAN bus is not valid");
        return;
    }

    canbus_handler_id_ = RegisterReceiveHandler(can_id_manager_->GetManagementCanId(), [this](const CanFrame& frame) {
        RunInSession([this, frame] {
            ProcessFrame(frame.data);
        });
    });

    if(canbus_handler_id_ < 0) {
        throw std::runtime_error(
            "Failed to register CAN frame handler for frame ID: "
            + std::to_string(can_id_manager_->GetManagementCanId().id)
        );
    }
}

void CdmpNetworkService::UnregisterCanHandlers() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    if(canbus_handler_id_ >= 0)
        (*canbus_)->RemoveFrameReceivedHandler(canbus_handler_id_);
    canbus_handler_id_ = -1;
}

void CdmpNetworkService::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    switch(new_status) {
        case CdmpDeviceStatus::INIT:
            RunInSession([this] {
                StartInitialization();
            });
            break;

        case CdmpDeviceStatus::CLAIMING:
            RunInSession([this] {
                StartIdClaim();
            });
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
    const CdmpManagementMessageType message_type = static_cast<CdmpManagementMessageType>(frame_data[0]);

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

        case CdmpManagementMessageType::HEARTBEAT:
            break;
    }
}

void CdmpNetworkService::StartInitialization() {
    discovery_attempt_ = 0;
    discovery_backoff_resets_ = 0;
    discovery_response_received_ = false;
    lower_uid_discovering_ = false;

    discovery_task_.value().Reschedule();
}

WorkQueueTaskResult CdmpNetworkService::ProcessDiscoveryAttempt(CdmpNetworkService* instance) {
    return instance->RunDiscoveryAttempt();
}

WorkQueueTaskResult CdmpNetworkService::RunDiscoveryAttempt() {
    if(device_->GetStatus() != CdmpDeviceStatus::INIT)
        return {};

    // The previous request's backoff has passed, and the frames received meanwhile are processed.
    if(discovery_attempt_ > 0) {
        if(discovery_response_received_) {
            device_->ClaimId();
            return {};
        }

        // A device with a lower UID discovering at the same time claims first, and answers us once online.
        if(lower_uid_discovering_ && discovery_backoff_resets_ < CdmpConstants::DISCOVERY_MAX_BACKOFF_RESETS) {
            discovery_backoff_resets_++;
            discovery_attempt_ = 0;
        }
    }

    if(discovery_attempt_ == CdmpTimeouts::RETRY_BACKOFFS_MS.size()) {
        device_->ClaimId();
        return {};
    }

    const int backoff = CdmpTimeouts::RETRY_BACKOFFS_MS[discovery_attempt_++];
    lower_uid_discovering_ = false;

    SendDiscoveryRequest();
    LOG_INF("Sending discovery request with backoff: %d ms", backoff);

    return { .reschedule = true, .delay = K_MSEC(backoff) };
}

void CdmpNetworkService::ProcessDiscoveryRequestFrame(std::span<const uint8_t> frame_data) {
    LOG_INF("Processing discovery request frame");

    try {
        const auto message = CdmpDiscoveryRequestMessage::FromCanFrame(frame_data);

        if(message.uid == 0 || message.uid == device_->GetUniqueIdentifier()) {
            LOG_ERR("Device UID conflict detected during discovery - received UID: %08X", message.uid);
        }

        if(device_->GetStatus() == CdmpDeviceStatus::INIT && message.uid < device_->GetUniqueIdentifier())
            lower_uid_discovering_ = true;

        RunInSession([this] {
            SendDiscoveryResponse();
        });
    } catch(const std::exception& e) {
        LOG_ERR("Error processing discovery request frame: %s", e.what());
    }
}

void CdmpNetworkService::ProcessDiscoveryResponseFrame(std::span<const uint8_t> frame_data) {
    LOG_INF("Processing discovery response frame");

    try {
        const auto message = CdmpDiscoveryResponseMessage::FromCanFrame(frame_data);
        discovery_response_received_ = true;

        if(message.uid == device_->GetUniqueIdentifier()) {
            LOG_ERR("Device UID conflict detected during discovery - received UID: %08X", message.uid);
            device_->EnterError();

            return;
        }

        UpdateDeviceFromDiscovery(message);
    } catch(const std::exception& e) {
        LOG_ERR("Error processing discovery response frame: %s", e.what());
    }
}

void CdmpNetworkService::SendDiscoveryRequest() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    CdmpDiscoveryRequestMessage message{};
    message.uid = device_->GetUniqueIdentifier();
    auto frame_data = message.ToCanFrame();
    CanId frame_id = can_id_manager_->GetDiscoveryRequestCanId();
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
    message.features = device_->GetFeatures();

    k_msleep(device_->GetStaggeredMessageDelay());

    auto frame_data = message.ToCanFrame();
    CanId frame_id = can_id_manager_->GetDiscoveryResponseCanId();
    (*canbus_)->SendFrame(frame_id, frame_data);

    LOG_INF("Sent discovery response for device %d", message.device_id);
}

void CdmpNetworkService::RequestRediscovery(uint8_t unknown_device_id) {
    // Before it is online, a unit learns the network through its own discovery.
    if(!device_->IsOnline() || GetState() != ServiceState::RUNNING)
        return;

    // One request brings back every device this unit does not know.
    const int64_t now = k_uptime_get();
    if(rediscovery_requested_at_ms_.has_value()
        && now - rediscovery_requested_at_ms_.value() < CdmpConstants::HEARTBEAT_INTERVAL_MS) {

        return;
    }

    rediscovery_requested_at_ms_ = now;
    LOG_INF("Heartbeat from unknown device %d, sending a discovery request", unknown_device_id);

    SendDiscoveryRequest();
}

void CdmpNetworkService::StartIdClaim() {
    claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
    id_claim_attempt_ = 0;
    id_claim_result_.reset();

    id_claim_task_.value().Reschedule();
}

WorkQueueTaskResult CdmpNetworkService::ProcessIdClaimAttempt(CdmpNetworkService* instance) {
    return instance->RunIdClaimAttempt();
}

WorkQueueTaskResult CdmpNetworkService::RunIdClaimAttempt() {
    if(device_->GetStatus() != CdmpDeviceStatus::CLAIMING) {
        claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
        return {};
    }

    if(!canbus_ || !canbus_->IsValid()) {
        LOG_ERR("Cannot claim a device ID: CAN bus is not valid");
        claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
        return {};
    }

    // Leaving CLAIMING for VERSION_MISMATCH or ERROR stops this service, and the stop waits for
    // this task, so those transitions run as separate work.
    if(id_claim_result_ == CdmpIdClaimResult::VERSION_INCOMPATIBLE) {
        LOG_ERR("ID claim refused: incompatible protocol version.");
        claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
        RunInSession([this] {
            device_->EnterVersionMismatch();
        });

        return {};
    }

    if(id_claim_result_ == CdmpIdClaimResult::ACCEPT) {
        CompleteIdClaim();
        return {};
    }

    if(claiming_device_id_ == CdmpDevice::DEVICE_ID_UNASSIGNED || id_claim_result_ == CdmpIdClaimResult::REJECT) {
        if(id_claim_result_ == CdmpIdClaimResult::REJECT)
            LOG_WRN("ID %d is taken, claiming the next free one.", claiming_device_id_);

        try {
            claiming_device_id_ = GetLowestAvailableId(claiming_device_id_);
        } catch(const std::exception& e) {
            LOG_ERR("Error getting lowest available ID: %s", e.what());
            claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
            RunInSession([this] {
                device_->EnterError();
            });

            return {};
        }

        id_claim_attempt_ = 0;
        id_claim_result_.reset();
    } else if(id_claim_attempt_ == CdmpConstants::ID_CLAIM_MAX_ATTEMPTS) {
        // Nobody objected: the first or only device on the network.
        CompleteIdClaim();
        return {};
    }

    SendIdClaim();
    id_claim_attempt_++;

    return { .reschedule = true, .delay = K_MSEC(CdmpConstants::ID_CLAIM_RESPONSE_TIMEOUT_MS) };
}

void CdmpNetworkService::CompleteIdClaim() {
    device_->SetDeviceId(claiming_device_id_);
    claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;

    UpdateNetworkDevices();
    device_->GoOnline();
}

void CdmpNetworkService::SendIdClaim() {
    CdmpIdClaimRequestMessage message = {};
    message.claiming_device_id = claiming_device_id_;
    message.uid = device_->GetUniqueIdentifier();
    message.protocol_version = device_->GetProtocolVersion();

    auto frame_data = message.ToCanFrame();
    CanId frame_id = can_id_manager_->GetIdClaimRequestCanId();
    (*canbus_)->SendFrame(frame_id, frame_data);
    LOG_INF("Sent ID claim for device %d", message.claiming_device_id);
}

void CdmpNetworkService::ProcessIdClaimRequestFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpIdClaimRequestMessage id_claim_message = CdmpIdClaimRequestMessage::FromCanFrame(frame_data);
        LOG_INF("Received ID claim from device %d", id_claim_message.claiming_device_id);

        RecordIdClaim(id_claim_message);

        if(!canbus_ || !canbus_->IsValid() || !device_->IsOnline())
            return;

        CdmpIdClaimResponseMessage message = {};
        message.responding_device_id = device_->GetDeviceId();
        message.claiming_device_id = id_claim_message.claiming_device_id;
        message.claiming_device_uid = id_claim_message.uid;

        bool send_response = false;

        if(device_->IsLowestIdOnNetwork() && !device_->IsProtocolCompatible(id_claim_message.protocol_version)) {

            message.result = CdmpIdClaimResult::VERSION_INCOMPATIBLE;
            send_response = true;
        } else if(device_->GetDeviceId() == id_claim_message.claiming_device_id) {
            message.result = CdmpIdClaimResult::REJECT;
            send_response = true;
        } else if(device_->IsLowestIdOnNetwork()) {
            // The claimant joins the table on its first heartbeat, as on every other unit.
            message.result = CdmpIdClaimResult::ACCEPT;
            send_response = true;
        }

        if(send_response) {
            auto frame_data = message.ToCanFrame();
            CanId response_id = can_id_manager_->GetIdClaimResponseCanId();
            (*canbus_)->SendFrame(response_id, frame_data);

            return;
        }
    } catch(const std::exception& e) {
        LOG_ERR("Error processing ID claim frame: %s", e.what());
    }
}

void CdmpNetworkService::ProcessIdClaimResponseFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpIdClaimResponseMessage response = CdmpIdClaimResponseMessage::FromCanFrame(frame_data);

        if(device_->GetStatus() != CdmpDeviceStatus::CLAIMING)
            return;

        if(response.claiming_device_id != claiming_device_id_)
            return;

        LOG_INF(
            "Received ID claim response from device %d for device %d with result %d", response.responding_device_id,
            response.claiming_device_id, static_cast<int>(response.result)
        );

        // The lowest ID accepts any claim, so the owner's objection to the same claim outweighs it.
        if(response.result != CdmpIdClaimResult::ACCEPT || !id_claim_result_.has_value())
            id_claim_result_ = response.result;
    } catch(const std::exception& e) {
        LOG_ERR("Error processing ID claim response frame: %s", e.what());
    }
}

void CdmpNetworkService::RecordIdClaim(const CdmpIdClaimRequestMessage& claim) {
    const uint8_t device_id = claim.claiming_device_id;

    // This unit's own claim, a claim on its ID that it rejects, or one refused for its protocol
    // version never becomes a device.
    if(!IsPeerDeviceId(device_id)
        || claim.uid == device_->GetUniqueIdentifier()
        || !device_->IsProtocolCompatible(claim.protocol_version)) {

        return;
    }

    std::optional<CdmpDeviceInfo> restarted;
    std::optional<uint8_t> previous_device_id;

    {
        ScopedMutex guard(devices_lock_);

        const auto it = network_devices_.find(device_id);
        if(it != network_devices_.end()) {
            // Another UID on a known ID is a conflicting claim, which the owner rejects.
            if(it->second->GetUniqueIdentifier() != claim.uid)
                return;

            // A claimant repeats its claim until it is answered; report the restart once.
            const PendingIdClaim* pending = FindPendingIdClaim(device_id);
            if(pending == nullptr || pending->uid != claim.uid)
                restarted = ToDeviceInfo(*it->second);

            // The claim proves the device is alive until its first heartbeat.
            it->second->UpdateHeartbeat();
        } else {
            // The same unit claiming another ID has given up its old one.
            previous_device_id = FindDeviceIdByUid(claim.uid);
        }

        StorePendingIdClaim(claim);
    }

    if(previous_device_id.has_value())
        RemoveDevice(previous_device_id.value());

    if(restarted.has_value()) {
        LOG_INF("Device %d restarted", device_id);
        NotifyDeviceEvent(CdmpDeviceEvent::RESTARTED, restarted.value());
    }
}

CdmpNetworkService::PendingIdClaim* CdmpNetworkService::FindPendingIdClaim(uint8_t device_id) {
    ExpirePendingIdClaims();

    auto* it = std::ranges::find(pending_id_claims_, device_id, &PendingIdClaim::device_id);

    return it != pending_id_claims_.end() ? &*it : nullptr;
}

void CdmpNetworkService::StorePendingIdClaim(const CdmpIdClaimRequestMessage& claim) {
    // A device claims one ID at a time; after a reject it moves on to the next.
    for(auto& pending : pending_id_claims_) {
        if(pending.uid == claim.uid && pending.device_id != claim.claiming_device_id)
            pending = {};
    }

    PendingIdClaim* slot = FindPendingIdClaim(claim.claiming_device_id);
    // Otherwise a free slot, or else the oldest claim's.
    if(slot == nullptr)
        slot = FindPendingIdClaim(CdmpDevice::DEVICE_ID_UNASSIGNED);
    if(slot == nullptr)
        slot = &*std::ranges::min_element(pending_id_claims_, {}, &PendingIdClaim::claimed_at_ms);

    *slot = { .device_id = claim.claiming_device_id, .uid = claim.uid, .claimed_at_ms = k_uptime_get() };
}

void CdmpNetworkService::ExpirePendingIdClaims() {
    const int64_t now = k_uptime_get();

    for(auto& claim : pending_id_claims_) {
        if(claim.device_id != CdmpDevice::DEVICE_ID_UNASSIGNED
            && now - claim.claimed_at_ms > CdmpConstants::HEARTBEAT_TIMEOUT_MS) {

            LOG_DBG("ID claim of device %d expired without a heartbeat", claim.device_id);
            claim = {};
        }
    }
}

std::optional<uint8_t> CdmpNetworkService::FindDeviceIdByUid(uint32_t uid) const {
    const auto it = std::ranges::find_if(network_devices_, [uid](const auto& entry) {
        return entry.second->GetUniqueIdentifier() == uid;
    });

    return it != network_devices_.end() ? std::optional<uint8_t>(it->first) : std::nullopt;
}

bool CdmpNetworkService::IsPeerDeviceId(uint8_t device_id) const {
    return device_id != CdmpDevice::DEVICE_ID_UNASSIGNED
        && device_id != CdmpDevice::DEVICE_ID_BROADCAST
        && !(device_->IsOnline() && device_id == device_->GetDeviceId());
}

void CdmpNetworkService::UpdateDeviceFromDiscovery(const CdmpDiscoveryResponseMessage& discovery) {
    if(!IsPeerDeviceId(discovery.device_id)) {
        LOG_WRN("Discovery response for device ID %d from UID 0x%08X ignored", discovery.device_id, discovery.uid);
        return;
    }

    std::optional<uint8_t> previous_device_id;
    {
        ScopedMutex guard(devices_lock_);

        previous_device_id = FindDeviceIdByUid(discovery.uid);
    }

    // The device restarted under another ID, and this unit missed its claim.
    if(previous_device_id.has_value() && previous_device_id.value() != discovery.device_id)
        RemoveDevice(previous_device_id.value());

    std::optional<CdmpDeviceInfo> joined;

    {
        ScopedMutex guard(devices_lock_);

        if(AddOrUpdateDevice(discovery.device_id, discovery.uid, discovery.features))
            joined = ToDeviceInfo(*network_devices_.at(discovery.device_id));

        // Only an online device answers discovery, so its claim, if any, is complete.
        PendingIdClaim* claim = FindPendingIdClaim(discovery.device_id);
        if(claim != nullptr)
            *claim = {};
    }

    if(joined.has_value())
        NotifyDeviceEvent(CdmpDeviceEvent::JOINED, joined.value());
}

void CdmpNetworkService::UpdateDeviceFromHeartbeat(const CdmpHeartbeatMessage& heartbeat) {
    const uint8_t device_id = heartbeat.device_id;

    if(!IsPeerDeviceId(device_id))
        return;

    std::optional<CdmpDeviceInfo> joined;
    bool is_known = true;

    {
        ScopedMutex guard(devices_lock_);

        PendingIdClaim* claim = FindPendingIdClaim(device_id);

        if(claim != nullptr) {
            if(AddOrUpdateDevice(device_id, claim->uid, heartbeat.features))
                joined = ToDeviceInfo(*network_devices_.at(device_id));

            *claim = {};
        } else if(network_devices_.contains(device_id)) {
            const auto& device = network_devices_.at(device_id);
            AddOrUpdateDevice(device_id, device->GetUniqueIdentifier(), heartbeat.features);
        } else {
            is_known = false;
        }
    }

    if(!is_known) {
        RequestRediscovery(device_id);
        return;
    }

    if(joined.has_value())
        NotifyDeviceEvent(CdmpDeviceEvent::JOINED, joined.value());

    NotifyHeartbeat(heartbeat);
}

bool CdmpNetworkService::AddOrUpdateDevice(uint8_t device_id, uint32_t uid, uint16_t features) {

    ScopedMutex guard(devices_lock_);

    if(network_devices_.contains(device_id)) {
        auto* device = network_devices_.at(device_id).get();

        device->UpdateHeartbeat();
        device->GoOnline(true);
        device->SetFeatures(features);

        LOG_DBG("Updated device %d.", device_id);

        return false;
    }

    auto device = std::make_unique<CdmpDevice>(uid, features, CdmpDeviceStatus::ONLINE);
    device->SetDeviceId(device_id);

    network_devices_.emplace(device_id, std::move(device));
    UpdateNetworkDevices();

    LOG_INF("Added new device: ID=%d, Features=0x%04X, UID=0x%08X", device_id, features, uid);

    return true;
}

void CdmpNetworkService::RemoveDevice(uint8_t device_id) {
    std::optional<CdmpDeviceInfo> removed;

    {
        ScopedMutex guard(devices_lock_);

        const auto it = network_devices_.find(device_id);
        if(it == network_devices_.end())
            return;

        removed = ToDeviceInfo(*it->second);
        network_devices_.erase(it);
        UpdateNetworkDevices();
        LOG_INF("Removed device %d", device_id);
    }

    NotifyDeviceEvent(CdmpDeviceEvent::LEFT, removed.value());
}

void CdmpNetworkService::ClearAllDevices() {
    ScopedMutex guard(devices_lock_);

    network_devices_.clear();
    pending_id_claims_.fill({});
    LOG_INF("Cleared all network devices");
}

int CdmpNetworkService::RegisterDeviceEventHandler(DeviceEventHandler handler) {
    ScopedMutex guard(handlers_lock_);

    const int handler_id = next_handler_id_++;
    device_event_handlers_.emplace(handler_id, std::move(handler));

    return handler_id;
}

void CdmpNetworkService::UnregisterDeviceEventHandler(int handler_id) {
    ScopedMutex guard(handlers_lock_);

    device_event_handlers_.erase(handler_id);
}

int CdmpNetworkService::RegisterHeartbeatHandler(HeartbeatHandler handler) {
    ScopedMutex guard(handlers_lock_);

    const int handler_id = next_handler_id_++;
    heartbeat_handlers_.emplace(handler_id, std::move(handler));

    return handler_id;
}

void CdmpNetworkService::UnregisterHeartbeatHandler(int handler_id) {
    ScopedMutex guard(handlers_lock_);

    heartbeat_handlers_.erase(handler_id);
}

void CdmpNetworkService::NotifyDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device) const {
    ScopedMutex guard(handlers_lock_);

    for(const auto& [_, handler] : device_event_handlers_)
        handler(event, device);
}

void CdmpNetworkService::NotifyHeartbeat(const CdmpHeartbeatMessage& heartbeat) const {
    ScopedMutex guard(handlers_lock_);

    for(const auto& [_, handler] : heartbeat_handlers_)
        handler(heartbeat);
}

std::vector<uint8_t> CdmpNetworkService::GetOnlineDeviceIds() const {
    ScopedMutex guard(devices_lock_);

    std::vector<uint8_t> online_devices;
    for(const auto& [device_id, device] : network_devices_) {
        if(device->GetStatus() == CdmpDeviceStatus::ONLINE)
            online_devices.push_back(device_id);
    }
    return online_devices;
}

std::vector<uint8_t> CdmpNetworkService::GetAllDeviceIds() const {
    ScopedMutex guard(devices_lock_);

    std::vector<uint8_t> all_devices;
    for(const auto& [device_id, device] : network_devices_)
        all_devices.push_back(device_id);

    return all_devices;
}

size_t CdmpNetworkService::GetDeviceCount() const {
    ScopedMutex guard(devices_lock_);

    return network_devices_.size();
}

size_t CdmpNetworkService::GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const {
    ScopedMutex guard(devices_lock_);

    size_t count = 0;
    for(const auto& [_, device] : network_devices_) {
        if(count == devices.size())
            break;

        devices[count++] = ToDeviceInfo(*device);
    }

    std::ranges::sort(devices.first(count), {}, &CdmpDeviceInfo::device_id);

    return count;
}

void CdmpNetworkService::SetAutoDiscovery(bool enabled) {
    auto_discovery_enabled_ = enabled;
    LOG_INF("Auto discovery %s", enabled ? "enabled" : "disabled");
}

WorkQueueTaskResult CdmpNetworkService::ProcessPeriodicValidation(CdmpNetworkService* instance) {
    if(!instance->is_validation_task_running_) {
        return { .reschedule = false };
    }

    instance->ProcessPeriodicTasks();

    return { .reschedule = instance->is_validation_task_running_,
        .delay = K_MSEC(CdmpConstants::NETWORK_VALIDATION_INTERVAL_MS) };
}

void CdmpNetworkService::ProcessPeriodicTasks() {
    RemoveOfflineDevices();
    UpdateNetworkDevices();
}

bool CdmpNetworkService::IsDeviceOnline(uint8_t device_id) const {
    ScopedMutex guard(devices_lock_);

    const auto it = network_devices_.find(device_id);
    return it != network_devices_.end() && it->second->GetStatus() == CdmpDeviceStatus::ONLINE;
}

void CdmpNetworkService::RemoveOfflineDevices() {
    std::vector<uint8_t> offline_devices;

    {
        ScopedMutex guard(devices_lock_);

        for(const auto& [device_id, device] : network_devices_) {
            if(device->GetLastHeartbeatDeltaMs() > CdmpConstants::HEARTBEAT_TIMEOUT_MS)
                offline_devices.push_back(device_id);
        }
    }

    // Outside the lock, so that the LEFT handlers may query the table.
    for(const auto device_id : offline_devices)
        RemoveDevice(device_id);
}

void CdmpNetworkService::UpdateNetworkDevices() {
    ScopedMutex guard(devices_lock_);

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

    device_->SetStaggeredMessageDelay(
        CdmpHelpers::CalculateStaggeredMessageTimeOffset(GetAllDeviceIds(), device_->GetDeviceId())
    );
    for(const auto& [id, device] : network_devices_)
        device->SetStaggeredMessageDelay(CdmpHelpers::CalculateStaggeredMessageTimeOffset(GetAllDeviceIds(), id));
}

uint8_t CdmpNetworkService::GetLowestAvailableId(uint8_t after) {
    ScopedMutex guard(devices_lock_);

    for(uint8_t i = after + 1; i < CdmpDevice::DEVICE_ID_BROADCAST; ++i) {
        // A pending claim holds its ID, which matters when units start together.
        if(!network_devices_.contains(i) && FindPendingIdClaim(i) == nullptr)
            return i;
    }

    throw std::runtime_error("No available device ID found");
}

void CdmpNetworkService::PrintNetworkStatus() const {
    ScopedMutex guard(devices_lock_);

    LOG_INF("Network Status: %zu devices total", network_devices_.size());

    for(const auto& [device_id, device] : network_devices_) {
        LOG_INF(
            "  Device %d: Features=0x%04X, Status=%d, UID=0x%08X", device_id, device->GetFeatures(),
            std::to_underlying(device->GetStatus()), device->GetUniqueIdentifier()
        );
    }
}

} // namespace eerie_leap::subsys::cdmp::services
