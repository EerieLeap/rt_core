#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/models/cdmp_state_data.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include "cdmp_canbus_service_base.h"
#include "cdmp_command_service.h"
#include "cdmp_network_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::cdmp::models::CdmpStateData;
using eerie_leap::subsys::cdmp::models::CdmpRemoteState;
using eerie_leap::subsys::cdmp::models::CdmpStateChangeMessage;
using eerie_leap::subsys::cdmp::models::CdmpStateChangeResponseMessage;

/**
 * @brief Shared state (§4.3): this unit's records, and its copies of every other unit's records.
 *
 * A record is a state type (1-255) with up to 5 data bytes that CDMP never interprets. Every change
 * of this unit's records increments one version counter, goes out as a notification, and is resent
 * after 50, 100 and 200 ms until every online peer has acknowledged it. A record that other changes
 * leave 64 versions behind is stamped and sent again, because receivers order a record's versions
 * within a window of 127. A receiver keeps the newest record per source and type, checks its copies
 * of a source against the digest in that source's heartbeat, and asks the source to replay its
 * records (STATE_SYNC_REQUEST) when they differ.
 *
 * Copies follow the device table: a source that claims its ID again (it restarted) makes them stale,
 * so the next record of each type is accepted whatever its version; a source that leaves the table
 * takes them along; and this unit leaving ONLINE drops them all. Notifications from a source that is
 * not in the table yet are not acknowledged, so the source resends them. This unit's own records
 * survive a stop and are announced on every ONLINE.
 *
 * The record API may be called from any thread. Frames, retransmissions and state handlers run on the
 * CDMP work queue, and receiving allocates nothing.
 */
class CdmpStateService final : public CdmpCanbusServiceBase {
public:
    /** @brief Called with a state type whose records of other units changed. */
    using StateHandler = std::function<void(uint8_t state_type)>;

private:
    static constexpr size_t MAX_LOCAL_RECORDS = CONFIG_EERIE_LEAP_CDMP_STATE_MAX_LOCAL_RECORDS;
    static constexpr size_t MAX_REMOTE_RECORDS = CONFIG_EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS;
    // A refresh lands at most LAG + 2 * records - 3 versions after the copy it replaces, since the
    // refreshes of one change follow each other; that has to stay within the window of 127.
    static_assert(CdmpConstants::STATE_VERSION_REFRESH_LAG + 2 * MAX_LOCAL_RECORDS - 3 <= 127,
        "Too many local records for the version refresh lag");
    // Frames waiting for the work queue; the digest recovers what a full queue drops.
    static constexpr size_t RX_QUEUE_SIZE = 16;

    struct LocalRecord {
        uint8_t state_type = 0;
        uint8_t version = 0;
        // Keeps its slot until the withdrawal is sent and acknowledged.
        bool is_withdrawn = false;
        bool is_announce_pending = false;
        CdmpStateData data;
        // Peers that have not acknowledged the current version yet.
        std::array<uint8_t, CdmpConstants::STATE_ACK_PEERS_MAX> unacknowledged{};
        uint8_t unacknowledged_count = 0;
        uint8_t retransmit_count = 0;
        int64_t next_retransmit_ms = 0;
    };

    struct RemoteRecord {
        uint8_t source = CdmpDevice::DEVICE_ID_UNASSIGNED;
        uint8_t state_type = 0;
        uint8_t version = 0;
        // Accepts the next record whatever its version.
        bool is_stale = false;
        CdmpStateData data;
    };

    struct SourceState {
        uint8_t device_id = CdmpDevice::DEVICE_ID_UNASSIGNED; // Unassigned marks a free slot.
        // Some of its records did not fit, so its copies never match its digest.
        bool is_overflowed = false;
        bool has_requested_sync = false;
        uint32_t sync_requested_at_ms = 0;
    };

    struct ReceivedFrame {
        bool is_response = false;
        uint8_t size = 0;
        CdmpStateChangeMessage::Frame data{};
    };

    struct StateHandlerEntry {
        uint8_t state_type = 0;
        StateHandler handler;
        bool is_first_call_pending = true;
    };

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CdmpNetworkService> network_service_;
    std::shared_ptr<CdmpCommandService> command_service_;

    int state_change_handler_id_ = -1;
    int state_change_response_handler_id_ = -1;
    int device_event_handler_id_ = -1;
    int heartbeat_handler_id_ = -1;

    // Filled by the CAN RX thread, drained by receive_task_.
    alignas(4) char rx_queue_buffer_[RX_QUEUE_SIZE * sizeof(ReceivedFrame)];
    k_msgq rx_queue_;
    atomic_t rx_dropped_ = ATOMIC_INIT(0);

    // Created by DoInitialize() and scheduled under records_lock_.
    std::optional<threading::WorkQueueTask<CdmpStateService>> receive_task_;
    std::optional<threading::WorkQueueTask<CdmpStateService>> transmit_task_;
    std::optional<threading::WorkQueueTask<CdmpStateService>> replay_task_;
    std::optional<threading::WorkQueueTask<CdmpStateService>> notify_task_;
    static WorkQueueTaskResult ProcessReceivedFrames(CdmpStateService* instance);
    static WorkQueueTaskResult ProcessTransmissions(CdmpStateService* instance);
    static WorkQueueTaskResult ProcessReplay(CdmpStateService* instance);
    static WorkQueueTaskResult ProcessStateHandlers(CdmpStateService* instance);

    // The records, never held across a call into the network or command service.
    mutable k_mutex records_lock_;
    // Cleared before DoStop() cancels the tasks, so that nothing schedules them again.
    bool is_started_ = false;
    uint8_t version_counter_ = 0;
    bool is_announce_all_pending_ = false;
    std::array<LocalRecord, MAX_LOCAL_RECORDS> local_records_{};
    size_t local_count_ = 0; // Sorted by type.
    std::array<RemoteRecord, MAX_REMOTE_RECORDS> remote_records_{};
    size_t remote_count_ = 0; // Sorted by source, then type.
    std::array<SourceState, MAX_REMOTE_RECORDS> sources_{};
    std::bitset<256> changed_types_;

    // Handlers run under it but without records_lock_, so they may read the records.
    mutable k_mutex handlers_lock_;
    int next_handler_id_ = 0;
    std::map<int, StateHandlerEntry> state_handlers_;

    void RegisterCanHandlers();
    void UnregisterCanHandlers();
    /** @brief On the CAN RX thread. */
    void QueueFrame(std::span<const uint8_t> frame_data, bool is_response);

    void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) override;
    void OnDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device);
    void OnHeartbeat(const CdmpHeartbeatMessage& heartbeat);

    // On the CDMP work queue.
    void ReceiveFrames();
    void Transmit();
    /** @return The number of records sent, or nothing when the service is stopped or not ONLINE. */
    std::optional<size_t> ReplayRecords();
    void NotifyStateHandlers();
    void SendNotification(const CdmpStateChangeMessage& message);
    void SendResponse(const CdmpStateChangeMessage& notification);

    // Under records_lock_.
    std::span<LocalRecord> LocalRecords() { return {local_records_.data(), local_count_}; }
    std::span<RemoteRecord> RemoteRecords() { return {remote_records_.data(), remote_count_}; }
    std::span<const RemoteRecord> RemoteRecords() const { return {remote_records_.data(), remote_count_}; }
    std::span<RemoteRecord> SourceRecords(uint8_t source);
    LocalRecord* FindLocalRecord(uint8_t state_type);
    /** @return The new record, or nullptr when every slot holds a record that is not withdrawn. */
    LocalRecord* AddLocalRecord(uint8_t state_type);
    void StampChange(LocalRecord& record);
    /** @brief Stamps again every record that other changes left STATE_VERSION_REFRESH_LAG behind. */
    void RefreshLaggingVersions();
    void UpdateDigest();
    void ApplyRecord(const CdmpStateChangeMessage& message);
    void RemoveFromAcknowledgementSets(uint8_t peer_id, std::optional<uint8_t> version = std::nullopt);
    void FreeAcknowledgedWithdrawals();
    void ScheduleRetransmission(int64_t now);
    /** @brief Removes the copies @p predicate selects and reports their types to the state handlers. */
    template<typename TPredicate>
    void RemoveRemoteRecords(TPredicate predicate);
    void DropSource(uint8_t source);
    /** @brief Drops every copy and stops retransmitting; this unit's records stay. */
    void ForgetNetwork();
    SourceState* FindSourceState(uint8_t source, bool add = false);
    void MarkOverflowed(uint8_t source);
    /** @brief Whether a sync request to @p source is due: at most one per heartbeat of the source. */
    bool StartSyncRequest(uint8_t source);
    void ScheduleStateHandlers();

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    CdmpStateService(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CdmpNetworkService> network_service,
        std::shared_ptr<CdmpCommandService> command_service);

    ~CdmpStateService();

    /**
     * @brief Shares @p data as this unit's record of @p state_type, from any thread.
     *
     * A new value is sent while the unit is ONLINE, and otherwise on its next ONLINE. Setting the
     * current value again sends nothing. Empty data withdraws the record, as on the wire.
     *
     * @return False for state type 0, more than 5 bytes, or no free record slot.
     */
    bool SetLocalState(uint8_t state_type, std::span<const uint8_t> data);
    /** @brief Withdraws this unit's record of @p state_type, from any thread. */
    void ClearLocalState(uint8_t state_type);

    /**
     * @brief Reports changes of other units' records of @p state_type.
     *
     * The handler runs on the CDMP work queue, once right after registration so that a late
     * subscriber catches up, and then whenever a record of that type is added, changed or removed.
     * It must not block, and must not register or unregister handlers.
     *
     * @return A handler ID for UnregisterStateHandler().
     */
    int RegisterStateHandler(uint8_t state_type, StateHandler handler);
    /** @brief Once this returns, the handler is not running and is not called again. */
    void UnregisterStateHandler(int handler_id);

    /**
     * @brief Copies other units' records of @p state_type, stale ones included, ordered by device ID.
     *
     * A span of MAX_REMOTE_RECORDS takes every record. A shorter one takes those of the lowest device
     * IDs and gives no sign of the rest.
     *
     * @return The number of records copied, at most @p states.size().
     */
    size_t GetRemoteStates(uint8_t state_type, std::span<CdmpRemoteState> states) const;

    /**
     * @brief Handles a notification (Base + 4), as the receive task does.
     *
     * A malformed frame, or one of type 0, is dropped without an exception and counted with the
     * frames the receive queue drops.
     */
    void ProcessStateChangeFrame(std::span<const uint8_t> frame_data);
    /** @brief Handles an acknowledgement (Base + 5), as the receive task does; drops a malformed one. */
    void ProcessStateChangeResponseFrame(std::span<const uint8_t> frame_data);
    /**
     * @brief Replays this unit's records for a STATE_SYNC_REQUEST.
     *
     * A unicast request is replayed at once and answered with SUCCESS and the record count; a
     * broadcast one is replayed after this unit's stagger and gets no response. A stopped service
     * replays nothing and does not answer.
     */
    std::optional<CdmpCommandResult> ProcessSyncRequest(const CdmpCommandRequestMessage& request);
};

} // namespace eerie_leap::subsys::cdmp::services
