#include <algorithm>
#include <limits>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "subsys/threading/scoped_mutex.h"

#include "cdmp_state_service.h"

LOG_MODULE_REGISTER(cdmp_state_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::utilities;
using namespace eerie_leap::subsys::cdmp::models;

using eerie_leap::subsys::threading::ScopedMutex;

namespace {

// Each record adds type, version, length and data, in ascending type order (§4.3).
uint8_t AddToDigest(uint8_t digest, uint8_t state_type, uint8_t version, const CdmpStateData& data) {
    const std::array<uint8_t, 3> header = { state_type, version, static_cast<uint8_t>(data.GetSize()) };
    digest = crc8_ccitt(digest, header.data(), header.size());

    return crc8_ccitt(digest, data.GetData().data(), data.GetSize());
}

// Serial number comparison, so that the 8-bit version may wrap.
bool IsNewer(uint8_t version, uint8_t current) {
    return static_cast<int8_t>(version - current) > 0;
}

} // namespace

CdmpStateService::CdmpStateService(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CdmpNetworkService> network_service,
    std::shared_ptr<CdmpCommandService> command_service
) :
    CdmpCanbusServiceBase(std::move(can_id_manager), std::move(device)),
    work_queue_thread_(std::move(work_queue_thread)),
    network_service_(std::move(network_service)),
    command_service_(std::move(command_service)) {

    k_mutex_init(&records_lock_);
    k_mutex_init(&handlers_lock_);
    k_msgq_init(&rx_queue_, rx_queue_buffer_, sizeof(ReceivedFrame), RX_QUEUE_SIZE);

    device_event_handler_id_ = network_service_->RegisterDeviceEventHandler(
        [this](CdmpDeviceEvent event, const CdmpDeviceInfo& device) {
            OnDeviceEvent(event, device);
        }
    );
    heartbeat_handler_id_ = network_service_->RegisterHeartbeatHandler([this](const CdmpHeartbeatMessage& heartbeat) {
        OnHeartbeat(heartbeat);
    });
    command_service_->RegisterServiceCommandHandler(
        CdmpServiceCommandCode::STATE_SYNC_REQUEST, [this](const CdmpCommandRequestMessage& request) {
            return ProcessSyncRequest(request);
        }
    );
}

CdmpStateService::~CdmpStateService() {
    // A destructor is noexcept: an exception from Stop() or a mutex would end in std::terminate.
    try {
        Stop();

        command_service_->UnregisterServiceCommandHandler(CdmpServiceCommandCode::STATE_SYNC_REQUEST);
        network_service_->UnregisterHeartbeatHandler(heartbeat_handler_id_);
        network_service_->UnregisterDeviceEventHandler(device_event_handler_id_);

        // Leaving ONLINE after the stop still reports the dropped copies. The others are cancelled too,
        // so that no timer outlives the service whatever state the stop found.
        for(auto* task : { &receive_task_, &transmit_task_, &replay_task_, &notify_task_ }) {
            if(task->has_value())
                task->value().Cancel();
        }
    } catch(const std::exception& e) {
        LOG_ERR("CDMP State Service teardown failed: %s", e.what());
    } catch(...) {
        LOG_ERR("CDMP State Service teardown failed.");
    }
}

bool CdmpStateService::DoInitialize() {
    ScopedMutex guard(records_lock_);

    receive_task_ = work_queue_thread_->CreateTask(ProcessReceivedFrames, this);
    transmit_task_ = work_queue_thread_->CreateTask(ProcessTransmissions, this);
    replay_task_ = work_queue_thread_->CreateTask(ProcessReplay, this);
    notify_task_ = work_queue_thread_->CreateTask(ProcessStateHandlers, this);

    // Handlers registered before the work queue ran get their first call now.
    notify_task_.value().Schedule();

    return true;
}

bool CdmpStateService::DoStart() {
    RegisterCanHandlers();

    {
        ScopedMutex guard(records_lock_);
        is_started_ = true;

        // Already ONLINE, so the transition that announces the records has passed.
        if(device_->IsOnline()) {
            is_announce_all_pending_ = true;
            if(transmit_task_.has_value())
                transmit_task_.value().Reschedule();
        }
    }

    LOG_INF("CDMP State Service started");

    return true;
}

bool CdmpStateService::DoStop() {
    UnregisterCanHandlers();

    {
        // A cancel does not stop a running task from arming its timer again, so nothing may
        // schedule a task once the cancels below begin. A Transmit() that already holds the lock
        // arms its timer before this, and the cancel removes it.
        ScopedMutex guard(records_lock_);
        is_started_ = false;
    }

    // No frame handler runs once they are unregistered, so this drops the last frames queued.
    if(receive_task_.has_value())
        receive_task_.value().Cancel();
    k_msgq_purge(&rx_queue_);

    if(transmit_task_.has_value())
        transmit_task_.value().Cancel();
    if(replay_task_.has_value())
        replay_task_.value().Cancel();

    {
        ScopedMutex guard(records_lock_);
        ForgetNetwork();
    }

    LOG_INF("CDMP State Service stopped");

    return true;
}

void CdmpStateService::RegisterCanHandlers() {
    if(!canbus_ || !canbus_->IsValid()) {
        LOG_ERR("Cannot register CAN handlers: CAN bus is not valid");
        return;
    }

    state_change_handler_id_ = RegisterReceiveHandler(
        can_id_manager_->GetStateChangeRequestCanId(), [this](const CanFrame& frame) {
            QueueFrame(frame.data, false);
        }
    );

    state_change_response_handler_id_ = RegisterReceiveHandler(
        can_id_manager_->GetStateChangeResponseCanId(), [this](const CanFrame& frame) {
            QueueFrame(frame.data, true);
        }
    );

    if(state_change_handler_id_ < 0 || state_change_response_handler_id_ < 0) {
        UnregisterCanHandlers();
        throw std::runtime_error("Failed to register the CAN frame handlers for state changes");
    }
}

void CdmpStateService::UnregisterCanHandlers() {
    if(!canbus_ || !canbus_->IsValid())
        return;

    if(state_change_handler_id_ >= 0)
        (*canbus_)->RemoveFrameReceivedHandler(state_change_handler_id_);
    if(state_change_response_handler_id_ >= 0)
        (*canbus_)->RemoveFrameReceivedHandler(state_change_response_handler_id_);

    state_change_handler_id_ = -1;
    state_change_response_handler_id_ = -1;
}

void CdmpStateService::QueueFrame(std::span<const uint8_t> frame_data, bool is_response) {
    ReceivedFrame frame{ .is_response = is_response };

    // Only a CAN FD frame can be longer, and no state frame is.
    if(frame_data.size() > frame.data.size()) {
        atomic_inc(&rx_dropped_);
        return;
    }

    frame.size = static_cast<uint8_t>(frame_data.size());
    std::ranges::copy(frame_data, frame.data.begin());

    if(k_msgq_put(&rx_queue_, &frame, K_NO_WAIT) != 0) {
        atomic_inc(&rx_dropped_);
        return;
    }

    if(receive_task_.has_value())
        receive_task_->Schedule();
}

WorkQueueTaskResult CdmpStateService::ProcessReceivedFrames(CdmpStateService* instance) {
    instance->ReceiveFrames();

    return {};
}

void CdmpStateService::ReceiveFrames() {
    ReceivedFrame frame;

    while(k_msgq_get(&rx_queue_, &frame, K_NO_WAIT) == 0) {
        const std::span<const uint8_t> frame_data(frame.data.data(), frame.size);

        if(frame.is_response)
            ProcessStateChangeResponseFrame(frame_data);
        else
            ProcessStateChangeFrame(frame_data);
    }

    const atomic_val_t dropped = atomic_clear(&rx_dropped_);
    if(dropped > 0)
        LOG_WRN("Dropped %ld state frames, malformed or with the queue full", static_cast<long>(dropped));
}

void CdmpStateService::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    ScopedMutex guard(records_lock_);

    if(new_status == CdmpDeviceStatus::ONLINE) {
        // Peers may have missed changes made while offline, or lost this unit's records altogether.
        is_announce_all_pending_ = true;
        if(is_started_ && transmit_task_.has_value())
            transmit_task_.value().Reschedule();
    } else if(old_status == CdmpDeviceStatus::ONLINE) {
        ForgetNetwork();
    }
}

void CdmpStateService::OnDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device) {
    ScopedMutex guard(records_lock_);

    switch(event) {
        case CdmpDeviceEvent::RESTARTED:
            // Its version counter started again, so a lower version is not an older record.
            for(auto& record : SourceRecords(device.device_id))
                record.is_stale = true;
            break;

        case CdmpDeviceEvent::LEFT:
            DropSource(device.device_id);
            break;

        default:
            break;
    }
}

void CdmpStateService::OnHeartbeat(const CdmpHeartbeatMessage& heartbeat) {
    const uint8_t source = heartbeat.device_id;
    bool request_sync = false;

    {
        ScopedMutex guard(records_lock_);

        if(!device_->IsOnline())
            return;

        // Its copies are incomplete, so they would never match.
        const SourceState* source_state = FindSourceState(source);
        if(source_state != nullptr && source_state->is_overflowed)
            return;

        auto records = SourceRecords(source);
        uint8_t digest = 0;
        uint8_t fresh_digest = 0;
        bool has_stale = false;

        for(const auto& record : records) {
            digest = AddToDigest(digest, record.state_type, record.version, record.data);

            if(record.is_stale)
                has_stale = true;
            else
                fresh_digest = AddToDigest(fresh_digest, record.state_type, record.version, record.data);
        }

        if(digest == heartbeat.state_digest) {
            for(auto& record : records)
                record.is_stale = false;
        } else if(has_stale && fresh_digest == heartbeat.state_digest) {
            // The replay brought back every record the source still has; it withdrew the others.
            RemoveRemoteRecords([source](const RemoteRecord& record) {
                return record.source == source && record.is_stale;
            });
        } else {
            for(auto& record : records)
                record.is_stale = true;

            request_sync = StartSyncRequest(source);
        }
    }

    if(request_sync) {
        LOG_INF("State of device %d differs from its digest, requesting a replay", source);
        command_service_->SendCommand(source, CdmpServiceCommandCode::STATE_SYNC_REQUEST);
    }
}

void CdmpStateService::ProcessStateChangeFrame(std::span<const uint8_t> frame_data) {
    if(!device_->IsOnline())
        return;

    // A foreign node on a shared bus may use these IDs, so a bad frame costs no exception and no
    // log line of its own; ReceiveFrames() reports the count.
    const auto message = CdmpStateChangeMessage::TryFromCanFrame(frame_data);
    if(!message.has_value() || message->state_type == 0) {
        atomic_inc(&rx_dropped_);
        return;
    }

    // Only a loopback controller hands a unit its own frames.
    if(message->source_device_id == device_->GetDeviceId())
        return;

    // Copies follow the device table, which reports their source leaving. A source that is not in it
    // yet, such as one whose first heartbeat is still on its way, gets no acknowledgement and resends.
    if(!network_service_->IsDeviceOnline(message->source_device_id)) {
        LOG_DBG("State change from unknown device %d ignored", message->source_device_id);
        return;
    }

    ApplyRecord(message.value());
    SendResponse(message.value());
}

void CdmpStateService::ProcessStateChangeResponseFrame(std::span<const uint8_t> frame_data) {
    if(!device_->IsOnline())
        return;

    const auto response = CdmpStateChangeResponseMessage::TryFromCanFrame(frame_data);
    if(!response.has_value()) {
        atomic_inc(&rx_dropped_);
        return;
    }

    const uint8_t own_id = device_->GetDeviceId();
    if(response->responding_device_id == own_id || response->source_device_id != own_id)
        return;

    ScopedMutex guard(records_lock_);
    RemoveFromAcknowledgementSets(response->responding_device_id, response->version);
}

std::optional<CdmpCommandResult> CdmpStateService::ProcessSyncRequest(const CdmpCommandRequestMessage& request) {
    if(request.target_device_id == CdmpDevice::DEVICE_ID_BROADCAST) {
        // Every unit answers a broadcast, each after its own stagger.
        ScopedMutex guard(records_lock_);
        if(is_started_ && replay_task_.has_value())
            replay_task_.value().Schedule(K_MSEC(device_->GetStaggeredMessageDelay()));

        return std::nullopt;
    }

    // Unanswered, the requester asks again at the next heartbeat.
    const auto count = ReplayRecords();
    if(!count.has_value())
        return std::nullopt;

    return CdmpCommandResult{ .result_code = CdmpResultCode::SUCCESS, .data = { static_cast<uint8_t>(count.value()) } };
}

WorkQueueTaskResult CdmpStateService::ProcessTransmissions(CdmpStateService* instance) {
    instance->Transmit();

    return {};
}

void CdmpStateService::Transmit() {
    std::array<CdmpDeviceInfo, CdmpConstants::STATE_ACK_PEERS_MAX> peers{};
    const size_t peer_count = network_service_->GetNetworkDevices(peers);

    std::array<CdmpStateChangeMessage, MAX_LOCAL_RECORDS> messages{};
    size_t message_count = 0;
    bool request_sync = false;

    {
        ScopedMutex guard(records_lock_);

        if(!is_started_ || !device_->IsOnline())
            return;

        const int64_t now = k_uptime_get();
        const auto& backoffs = CdmpTimeouts::STATE_RETRANSMIT_BACKOFFS_MS;

        if(is_announce_all_pending_) {
            is_announce_all_pending_ = false;
            request_sync = true;

            for(auto& record : LocalRecords())
                record.is_announce_pending = true;
        }

        for(auto& record : LocalRecords()) {
            if(record.is_announce_pending) {
                record.is_announce_pending = false;
                record.retransmit_count = 0;
                record.unacknowledged_count = 0;
                for(const auto& peer : std::span(peers).first(peer_count)) {
                    if(peer.status == CdmpDeviceStatus::ONLINE)
                        record.unacknowledged[record.unacknowledged_count++] = peer.device_id;
                }
                record.next_retransmit_ms = now + backoffs[0];
            } else if(record.unacknowledged_count > 0 && now >= record.next_retransmit_ms) {
                record.retransmit_count++;

                // After the last resend the heartbeat digest takes over.
                if(record.retransmit_count == backoffs.size())
                    record.unacknowledged_count = 0;
                else
                    record.next_retransmit_ms = now + backoffs[record.retransmit_count];
            } else {
                continue;
            }

            // The record's current value, which a newer change replaces.
            messages[message_count++] = { .source_device_id = device_->GetDeviceId(),
                .version = record.version,
                .state_type = record.state_type,
                .data = record.data };
        }

        FreeAcknowledgedWithdrawals();
        ScheduleRetransmission(now);
    }

    for(const auto& message : std::span(messages).first(message_count))
        SendNotification(message);

    if(request_sync)
        command_service_->SendCommand(CdmpDevice::DEVICE_ID_BROADCAST, CdmpServiceCommandCode::STATE_SYNC_REQUEST);
}

WorkQueueTaskResult CdmpStateService::ProcessReplay(CdmpStateService* instance) {
    instance->ReplayRecords();

    return {};
}

std::optional<size_t> CdmpStateService::ReplayRecords() {
    std::array<CdmpStateChangeMessage, MAX_LOCAL_RECORDS> messages{};
    size_t message_count = 0;

    {
        ScopedMutex guard(records_lock_);

        if(!is_started_ || !device_->IsOnline())
            return std::nullopt;

        // Not tracked: a requester that misses the replay asks again.
        for(const auto& record : LocalRecords()) {
            if(record.is_withdrawn)
                continue;

            messages[message_count++] = { .source_device_id = device_->GetDeviceId(),
                .version = record.version,
                .state_type = record.state_type,
                .data = record.data };
        }
    }

    for(const auto& message : std::span(messages).first(message_count))
        SendNotification(message);

    LOG_DBG("Replayed %zu state records", message_count);

    return message_count;
}

void CdmpStateService::SendNotification(const CdmpStateChangeMessage& message) {
    if(!canbus_ || !canbus_->IsValid())
        return;

    CdmpStateChangeMessage::Frame frame{};
    const size_t size = message.ToCanFrame(frame);

    (*canbus_)->SendFrame(can_id_manager_->GetStateChangeRequestCanId(), std::span(frame).first(size));
}

void CdmpStateService::SendResponse(const CdmpStateChangeMessage& notification) {
    if(!canbus_ || !canbus_->IsValid())
        return;

    // Duplicates too, so that a sender whose acknowledgement was lost stops resending.
    const CdmpStateChangeResponseMessage response{ .responding_device_id = device_->GetDeviceId(),
        .source_device_id = notification.source_device_id,
        .version = notification.version,
        .result_code = CdmpResultCode::SUCCESS };

    (*canbus_)->SendFrame(can_id_manager_->GetStateChangeResponseCanId(), response.ToCanFrame());
}

WorkQueueTaskResult CdmpStateService::ProcessStateHandlers(CdmpStateService* instance) {
    instance->NotifyStateHandlers();

    return {};
}

void CdmpStateService::NotifyStateHandlers() {
    std::bitset<256> changed_types;
    {
        ScopedMutex guard(records_lock_);
        changed_types = std::exchange(changed_types_, {});
    }

    ScopedMutex guard(handlers_lock_);

    for(auto& [_, entry] : state_handlers_) {
        if(!entry.is_first_call_pending && !changed_types.test(entry.state_type))
            continue;

        entry.is_first_call_pending = false;

        try {
            entry.handler(entry.state_type);
        } catch(const std::exception& e) {
            LOG_ERR("State handler for type 0x%02X threw: %s", entry.state_type, e.what());
        }
    }
}

bool CdmpStateService::SetLocalState(uint8_t state_type, std::span<const uint8_t> data) {
    if(data.empty()) {
        ClearLocalState(state_type);
        return state_type != 0;
    }

    if(state_type == 0 || data.size() > CdmpStateData::MAX_SIZE)
        return false;

    const CdmpStateData state_data(data);

    ScopedMutex guard(records_lock_);

    LocalRecord* record = FindLocalRecord(state_type);
    if(record != nullptr && !record->is_withdrawn && record->data == state_data)
        return true;

    if(record == nullptr)
        record = AddLocalRecord(state_type);

    if(record == nullptr) {
        LOG_WRN("No room for state type 0x%02X: all %zu records are in use", state_type, MAX_LOCAL_RECORDS);
        return false;
    }

    record->is_withdrawn = false;
    record->data = state_data;
    StampChange(*record);

    return true;
}

void CdmpStateService::ClearLocalState(uint8_t state_type) {
    ScopedMutex guard(records_lock_);

    LocalRecord* record = FindLocalRecord(state_type);
    if(record == nullptr || record->is_withdrawn)
        return;

    record->is_withdrawn = true;
    record->data = {};
    StampChange(*record);
}

int CdmpStateService::RegisterStateHandler(uint8_t state_type, StateHandler handler) {
    int handler_id = 0;

    {
        ScopedMutex guard(handlers_lock_);

        handler_id = next_handler_id_++;
        state_handlers_.emplace(
            handler_id, StateHandlerEntry{ .state_type = state_type, .handler = std::move(handler) }
        );
    }

    // Before DoInitialize() the work queue may not run yet, which then schedules the first call.
    ScopedMutex guard(records_lock_);
    if(notify_task_.has_value())
        notify_task_.value().Schedule();

    return handler_id;
}

void CdmpStateService::UnregisterStateHandler(int handler_id) {
    ScopedMutex guard(handlers_lock_);

    state_handlers_.erase(handler_id);
}

size_t CdmpStateService::GetRemoteStates(uint8_t state_type, std::span<CdmpRemoteState> states) const {
    ScopedMutex guard(records_lock_);

    size_t count = 0;
    for(const auto& record : RemoteRecords()) {
        if(count == states.size())
            break;

        if(record.state_type == state_type)
            states[count++] = { .device_id = record.source, .data = record.data, .is_stale = record.is_stale };
    }

    return count;
}

std::span<CdmpStateService::RemoteRecord> CdmpStateService::SourceRecords(uint8_t source) {
    const auto range = std::ranges::equal_range(RemoteRecords(), source, {}, &RemoteRecord::source);

    return { range.begin(), range.end() };
}

CdmpStateService::LocalRecord* CdmpStateService::FindLocalRecord(uint8_t state_type) {
    const auto records = LocalRecords();
    const auto it = std::ranges::lower_bound(records, state_type, {}, &LocalRecord::state_type);

    return it != records.end() && it->state_type == state_type ? &*it : nullptr;
}

CdmpStateService::LocalRecord* CdmpStateService::AddLocalRecord(uint8_t state_type) {
    if(local_count_ == local_records_.size()) {
        // A withdrawal still waiting for acknowledgements gives way; the digest covers it.
        const auto records = LocalRecords();
        const auto withdrawn = std::ranges::find(records, true, &LocalRecord::is_withdrawn);
        if(withdrawn == records.end())
            return nullptr;

        std::move(withdrawn + 1, records.end(), withdrawn);
        local_count_--;
    }

    const auto records = LocalRecords();
    const auto index = std::ranges::lower_bound(records, state_type, {}, &LocalRecord::state_type) - records.begin();
    auto* const first = local_records_.begin();
    auto* const position = first + index;

    std::move_backward(position, first + local_count_, first + local_count_ + 1);
    local_count_++;

    *position = { .state_type = state_type };

    return &*position;
}

void CdmpStateService::StampChange(LocalRecord& record) {
    record.version = ++version_counter_;
    // A newer change replaces the acknowledgement set of the previous one.
    record.unacknowledged_count = 0;
    record.is_announce_pending = true;

    RefreshLaggingVersions();
    UpdateDigest();

    if(is_started_ && transmit_task_.has_value() && device_->IsOnline())
        transmit_task_.value().Reschedule();
}

void CdmpStateService::RefreshLaggingVersions() {
    // Its current value under a new version, sent and acknowledged like a change. A withdrawn record
    // has no copies to compare with.
    for(auto& record : LocalRecords()) {
        const auto lag = static_cast<uint8_t>(version_counter_ - record.version);
        if(record.is_withdrawn || lag < CdmpConstants::STATE_VERSION_REFRESH_LAG)
            continue;

        record.version = ++version_counter_;
        record.unacknowledged_count = 0;
        record.is_announce_pending = true;
    }
}

void CdmpStateService::UpdateDigest() {
    uint8_t digest = 0;

    for(const auto& record : LocalRecords()) {
        if(!record.is_withdrawn)
            digest = AddToDigest(digest, record.state_type, record.version, record.data);
    }

    device_->SetStateDigest(digest);
}

void CdmpStateService::ApplyRecord(const CdmpStateChangeMessage& message) {
    ScopedMutex guard(records_lock_);

    const uint8_t source = message.source_device_id;
    const auto records = RemoteRecords();
    const auto it = std::ranges::lower_bound(
        records, std::pair(source, message.state_type), {}, [](const RemoteRecord& record) {
            return std::pair(record.source, record.state_type);
        }
    );
    const bool is_found = it != records.end() && it->source == source && it->state_type == message.state_type;

    // A duplicate, or a resend overtaken by a newer change.
    if(is_found && !it->is_stale && !IsNewer(message.version, it->version))
        return;

    if(message.data.IsEmpty()) {
        if(is_found) {
            RemoveRemoteRecords([source, state_type = message.state_type](const RemoteRecord& record) {
                return record.source == source && record.state_type == state_type;
            });
        }

        return;
    }

    if(is_found) {
        if(it->data != message.data) {
            changed_types_.set(message.state_type);
            ScheduleStateHandlers();
        }

        it->version = message.version;
        it->data = message.data;
        it->is_stale = false;

        return;
    }

    if(remote_count_ == remote_records_.size()) {
        MarkOverflowed(source);
        return;
    }

    auto* const first = remote_records_.begin();
    auto* const position = first + (it - records.begin());

    std::move_backward(position, first + remote_count_, first + remote_count_ + 1);
    remote_count_++;

    *position = {
        .source = source, .state_type = message.state_type, .version = message.version, .data = message.data
    };

    changed_types_.set(message.state_type);
    ScheduleStateHandlers();
}

void CdmpStateService::RemoveFromAcknowledgementSets(uint8_t peer_id, std::optional<uint8_t> version) {
    for(auto& record : LocalRecords()) {
        if(version.has_value() && record.version != version.value())
            continue;

        const auto unacknowledged = std::span(record.unacknowledged).first(record.unacknowledged_count);
        const auto it = std::ranges::find(unacknowledged, peer_id);
        if(it == unacknowledged.end())
            continue;

        *it = unacknowledged.back();
        record.unacknowledged_count--;
    }

    FreeAcknowledgedWithdrawals();
}

void CdmpStateService::FreeAcknowledgedWithdrawals() {
    const auto records = LocalRecords();
    const auto freed = std::ranges::remove_if(records, [](const LocalRecord& record) {
        return record.is_withdrawn && !record.is_announce_pending && record.unacknowledged_count == 0;
    });

    local_count_ -= freed.size();
}

void CdmpStateService::ScheduleRetransmission(int64_t now) {
    int64_t next_retransmit_ms = std::numeric_limits<int64_t>::max();

    for(const auto& record : LocalRecords()) {
        if(record.unacknowledged_count > 0)
            next_retransmit_ms = std::min(next_retransmit_ms, record.next_retransmit_ms);
    }

    // Under records_lock_, like every other reschedule, so the latest one is always right.
    if(next_retransmit_ms != std::numeric_limits<int64_t>::max() && transmit_task_.has_value())
        transmit_task_->Reschedule(K_MSEC(std::max<int64_t>(next_retransmit_ms - now, 0)));
}

template<typename TPredicate>
void CdmpStateService::RemoveRemoteRecords(TPredicate predicate) {
    const auto records = RemoteRecords();

    for(const auto& record : records) {
        if(predicate(record))
            changed_types_.set(record.state_type);
    }

    const auto removed = std::ranges::remove_if(records, predicate);
    if(removed.empty())
        return;

    remote_count_ -= removed.size();

    // A source that did not fit may fit now; its next digest mismatch fills it in.
    for(auto& source_state : sources_)
        source_state.is_overflowed = false;

    ScheduleStateHandlers();
}

void CdmpStateService::DropSource(uint8_t source) {
    RemoveRemoteRecords([source](const RemoteRecord& record) {
        return record.source == source;
    });
    RemoveFromAcknowledgementSets(source);

    SourceState* source_state = FindSourceState(source);
    if(source_state != nullptr)
        *source_state = {};
}

void CdmpStateService::ForgetNetwork() {
    RemoveRemoteRecords([](const RemoteRecord&) {
        return true;
    });
    sources_.fill({});

    is_announce_all_pending_ = false;
    for(auto& record : LocalRecords())
        record.unacknowledged_count = 0;
}

CdmpStateService::SourceState* CdmpStateService::FindSourceState(uint8_t source, bool add) {
    auto* it = std::ranges::find(sources_, source, &SourceState::device_id);
    if(it != sources_.end())
        return &*it;

    if(!add)
        return nullptr;

    // A free slot, or one that holds no more than an expired rate limit.
    const uint32_t now = k_uptime_get_32();
    it = std::ranges::find_if(sources_, [now](const SourceState& source_state) {
        return source_state.device_id == CdmpDevice::DEVICE_ID_UNASSIGNED
            || (!source_state.is_overflowed
                && (!source_state.has_requested_sync
                    || now - source_state.sync_requested_at_ms >= CdmpConstants::STATE_SYNC_REQUEST_MIN_INTERVAL_MS));
    });
    if(it == sources_.end())
        return nullptr;

    *it = { .device_id = source };

    return &*it;
}

void CdmpStateService::MarkOverflowed(uint8_t source) {
    SourceState* source_state = FindSourceState(source, true);
    if(source_state == nullptr || source_state->is_overflowed)
        return;

    source_state->is_overflowed = true;
    LOG_WRN(
        "No room for more state records of device %d: keeping %zu records in all, without its digest check", source,
        remote_count_
    );
}

bool CdmpStateService::StartSyncRequest(uint8_t source) {
    SourceState* source_state = FindSourceState(source, true);
    if(source_state == nullptr)
        return false;

    const uint32_t now = k_uptime_get_32();
    if(source_state->has_requested_sync
        && now - source_state->sync_requested_at_ms < CdmpConstants::STATE_SYNC_REQUEST_MIN_INTERVAL_MS) {

        return false;
    }

    source_state->has_requested_sync = true;
    source_state->sync_requested_at_ms = now;

    return true;
}

void CdmpStateService::ScheduleStateHandlers() {
    if(notify_task_.has_value())
        notify_task_.value().Schedule();
}

} // namespace eerie_leap::subsys::cdmp::services
