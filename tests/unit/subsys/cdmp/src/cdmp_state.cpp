#include <algorithm>
#include <array>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "cdmp_test_features.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/cdmp/models/messages/cdmp_command_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_discovery_response_message.h"
#include "subsys/cdmp/models/messages/cdmp_heartbeat_message.h"
#include "subsys/cdmp/models/messages/cdmp_id_claim_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_state_change_message.h"
#include "subsys/cdmp/models/messages/cdmp_state_change_response_message.h"
#include "subsys/cdmp/services/cdmp_command_service.h"
#include "subsys/cdmp/services/cdmp_network_service.h"
#include "subsys/cdmp/services/cdmp_state_service.h"
#include "subsys/threading/work_queue_thread.h"

using eerie_leap::tests::DISPLAY_FEATURES;
using eerie_leap::tests::LOGGER_FEATURES;

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services;
using namespace eerie_leap::subsys::cdmp::utilities;

using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr uint32_t OWN_UID = 0x11111111;
constexpr uint8_t OWN_ID = 1;
constexpr uint8_t PEER_ID = 4;
constexpr uint32_t PEER_UID = 0x44444444;
constexpr uint8_t OTHER_PEER_ID = 5;
constexpr uint32_t OTHER_PEER_UID = 0x55555555;
constexpr uint8_t TYPE_A = 0x10;
constexpr uint8_t TYPE_B = 0x11;

constexpr int FRAME_TIMEOUT_MS = 1000;
// Well past the time a frame takes through the loopback controller.
constexpr int NO_FRAME_TIMEOUT_MS = 100;
// Past the third resend, 50 + 100 + 200 ms after a change.
constexpr int RESEND_WINDOW_MS = 400;
// A wait on a timer may end a tick late, and the uptime it is measured with has tick resolution.
constexpr int TIMING_TOLERANCE_BELOW_MS = 10;

using Bytes = std::vector<uint8_t>;

const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the fixture teardown, so the controller is stopped separately.
void ResetLoopbackDevice(void* /*fixture*/) {
    can_stop(LoopbackDevice());
}

Bytes NotificationFrame(uint8_t source, uint8_t version, uint8_t state_type, const Bytes& data) {
    const CdmpStateChangeMessage message{
        .source_device_id = source, .version = version, .state_type = state_type, .data = CdmpStateData(data)
    };

    CdmpStateChangeMessage::Frame frame{};
    const size_t size = message.ToCanFrame(frame);

    return { frame.begin(), frame.begin() + size };
}

Bytes ToBytes(const CdmpStateData& data) {
    return { data.GetData().begin(), data.GetData().end() };
}

CdmpCommandRequestMessage SyncRequest(uint8_t target) {
    return { .target_device_id = target,
        .command_code = std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST),
        .transaction_id = 0x42,
        .data = {} };
}

struct Record {
    uint8_t state_type;
    uint8_t version;
    Bytes data;
};

// Written out from the spec in one pass, so it also checks that the service's chained CRC agrees.
uint8_t Digest(std::initializer_list<Record> records) {
    Bytes bytes;
    for(const auto& record : records) {
        bytes.push_back(record.state_type);
        bytes.push_back(record.version);
        bytes.push_back(static_cast<uint8_t>(record.data.size()));
        bytes.insert(bytes.end(), record.data.begin(), record.data.end());
    }

    return crc8_ccitt(0, bytes.data(), bytes.size());
}

// Frames seen on the loopback bus, one queue per CDMP offset.
class BusFrames {
private:
    struct Channel {
        k_sem received{};
        std::deque<Bytes> frames;
    };

    k_mutex lock_{};
    std::array<Channel, CdmpCanIdManager::STATE_CHANGE_RESPONSE_OFFSET + 1> channels_;

public:
    BusFrames() {
        k_mutex_init(&lock_);
        for(auto& channel : channels_)
            k_sem_init(&channel.received, 0, K_SEM_MAX_LIMIT);
    }

    void Add(uint32_t offset, const CanFrame& frame) {
        Channel& channel = channels_.at(offset);

        k_mutex_lock(&lock_, K_FOREVER);
        channel.frames.emplace_back(frame.data.begin(), frame.data.end());
        k_mutex_unlock(&lock_);

        k_sem_give(&channel.received);
    }

    std::optional<Bytes> Wait(uint32_t offset, int timeout_ms = FRAME_TIMEOUT_MS) {
        Channel& channel = channels_.at(offset);
        if(k_sem_take(&channel.received, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        Bytes frame = std::move(channel.frames.front());
        channel.frames.pop_front();
        k_mutex_unlock(&lock_);

        return frame;
    }
};

struct StateFixture {
    std::shared_ptr<CdmpDevice> device = std::make_shared<CdmpDevice>(OWN_UID, DISPLAY_FEATURES);
    std::shared_ptr<WorkQueueThread> work_queue = std::make_shared<WorkQueueThread>(
        "cdmp_test_wq",
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE,
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_PRIORITY
    );
    std::shared_ptr<CdmpCanIdManager> can_id_manager = std::make_shared<CdmpCanIdManager>();

    // The services get the bus after Start(), without receive handlers, and the tests hand them
    // frames on the test thread.
    std::shared_ptr<CdmpNetworkService> network =
        std::make_shared<CdmpNetworkService>(can_id_manager, device, work_queue);
    std::shared_ptr<CdmpCommandService> commands =
        std::make_shared<CdmpCommandService>(can_id_manager, device, work_queue);
    std::shared_ptr<CdmpStateService> state =
        std::make_shared<CdmpStateService>(can_id_manager, device, work_queue, network, commands);

    std::shared_ptr<CanbusProxy> canbus;
    std::vector<int> bus_handler_ids;
    BusFrames bus_frames;

    StateFixture() {
        zassert_true(work_queue->Initialize());
        for(ICdmpCanbusService* service : Services()) {
            zassert_true(service->Initialize());
            zassert_true(service->Start());
        }

        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(LoopbackDevice(), CanbusType::CLASSICAL_CAN, 500000, 0, CAN_MODE_LOOPBACK)
        );
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        // The services may share one block filter for their IDs, which a different filter over the
        // same IDs would overlap, so the recorder registers the same filters and sorts by ID.
        constexpr std::array recorded_offsets{ CdmpCanIdManager::COMMAND_OFFSET, CdmpCanIdManager::STATE_CHANGE_OFFSET,
            CdmpCanIdManager::STATE_CHANGE_RESPONSE_OFFSET };

        std::vector<CanFilter> filters;
        for(const uint32_t offset : recorded_offsets) {
            const CanFilter filter = can_id_manager->GetReceiveFilter(
                CanId::Standard(can_id_manager->GetBaseCanId() + offset)
            );
            if(std::find(filters.begin(), filters.end(), filter) == filters.end())
                filters.push_back(filter);
        }

        for(const CanFilter& filter : filters) {
            const int handler_id = (*canbus)->RegisterFrameReceivedHandler(
                filter, [this, recorded_offsets](const CanFrame& frame) {
                    const uint32_t offset = frame.id - can_id_manager->GetBaseCanId();
                    if(std::find(recorded_offsets.begin(), recorded_offsets.end(), offset) != recorded_offsets.end())
                        bus_frames.Add(offset, frame);
                }
            );
            zassert_true(handler_id > 0, "Frame handler rejected: %d", handler_id);
            bus_handler_ids.push_back(handler_id);
        }

        for(ICdmpCanbusService* service : Services())
            service->Configure(canbus);
    }

    ~StateFixture() {
        for(const int handler_id : bus_handler_ids)
            (*canbus)->RemoveFrameReceivedHandler(handler_id);

        // Queued work must run while the services exist; after the stop it does nothing.
        for(ICdmpCanbusService* service : Services())
            service->Stop();
        work_queue->Stop();
    }

    std::array<ICdmpCanbusService*, 3> Services() {
        return { network.get(), commands.get(), state.get() };
    }

    /** @brief Returns once the work queued so far has run. */
    void DrainQueue() {
        k_sem done;
        k_sem_init(&done, 0, 1);
        work_queue->Run([&done]() {
            k_sem_give(&done);
        });
        zassert_equal(k_sem_take(&done, K_MSEC(FRAME_TIMEOUT_MS)), 0, "The work queue is stuck");
    }

    // Consumes the sync request this unit broadcasts on going online.
    void GoOnline(uint8_t device_id = OWN_ID) {
        device->SetDeviceId(device_id);
        device->GoOnline(true);
        network->ProcessPeriodicTasks();

        auto request = WaitForSyncRequest();
        zassert_true(request.has_value(), "Going online broadcasts STATE_SYNC_REQUEST");
        zassert_equal(request->target_device_id, CdmpDevice::DEVICE_ID_BROADCAST);
    }

    void AddPeer(uint8_t device_id, uint32_t uid) {
        network->ProcessFrame(
            CdmpDiscoveryResponseMessage{ .device_id = device_id, .uid = uid, .features = LOGGER_FEATURES }.ToCanFrame()
        );
    }

    void ClaimAgain(uint8_t device_id, uint32_t uid) {
        network->ProcessFrame(
            CdmpIdClaimRequestMessage{ .claiming_device_id = device_id,
                .uid = uid,
                .protocol_version = CdmpDevice::GetCurrentProtocolVersion() }
                .ToCanFrame()
        );
    }

    void Receive(uint8_t source, uint8_t version, uint8_t state_type, const Bytes& data) {
        state->ProcessStateChangeFrame(NotificationFrame(source, version, state_type, data));
    }

    void Acknowledge(uint8_t responder, uint8_t version) {
        state->ProcessStateChangeResponseFrame(
            CdmpStateChangeResponseMessage{ .responding_device_id = responder,
                .source_device_id = OWN_ID,
                .version = version,
                .result_code = CdmpResultCode::SUCCESS }
                .ToCanFrame()
        );
    }

    // Through the network service, which passes it on to the state service.
    void Heartbeat(uint8_t device_id, uint8_t state_digest) {
        network->UpdateDeviceFromHeartbeat(
            { .device_id = device_id,
                .health_status = CdmpHealthStatus::OK,
                .sequence_number = 0,
                .features = 0,
                .state_digest = state_digest }
        );
    }

    std::vector<CdmpRemoteState> Remote(uint8_t state_type) const {
        std::vector<CdmpRemoteState> states(CONFIG_EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS);
        states.resize(state->GetRemoteStates(state_type, states));

        return states;
    }

    bool HasRemote(uint8_t device_id, uint8_t state_type, const Bytes& data) const {
        for(const auto& remote : Remote(state_type)) {
            if(remote.device_id == device_id)
                return remote.data == CdmpStateData(data);
        }

        return false;
    }

    std::optional<CdmpStateChangeResponseMessage> WaitForAcknowledgement(int timeout_ms = FRAME_TIMEOUT_MS) {
        auto frame = bus_frames.Wait(CdmpCanIdManager::STATE_CHANGE_RESPONSE_OFFSET, timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        return CdmpStateChangeResponseMessage::FromCanFrame(*frame);
    }

    std::optional<CdmpStateChangeMessage> WaitForNotification(int timeout_ms = FRAME_TIMEOUT_MS) {
        auto frame = bus_frames.Wait(CdmpCanIdManager::STATE_CHANGE_OFFSET, timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        return CdmpStateChangeMessage::FromCanFrame(*frame);
    }

    std::optional<CdmpCommandRequestMessage> WaitForSyncRequest(int timeout_ms = FRAME_TIMEOUT_MS) {
        auto frame = bus_frames.Wait(CdmpCanIdManager::COMMAND_OFFSET, timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        auto request = CdmpCommandRequestMessage::FromCanFrame(*frame);
        zassert_equal(request.command_code, std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST));

        return request;
    }
};

} // namespace

ZTEST_SUITE(cdmp_state, NULL, NULL, NULL, ResetLoopbackDevice, NULL);

ZTEST(cdmp_state, test_notification_from_a_peer_is_acknowledged_and_applied) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01, 0x02 });

    auto acknowledgement = fixture.WaitForAcknowledgement();
    zassert_true(acknowledgement.has_value());
    zassert_equal(acknowledgement->responding_device_id, OWN_ID);
    zassert_equal(acknowledgement->source_device_id, PEER_ID);
    zassert_equal(acknowledgement->version, 1);
    zassert_equal(acknowledgement->result_code, CdmpResultCode::SUCCESS);

    const auto remote = fixture.Remote(TYPE_A);
    zassert_equal(remote.size(), 1);
    zassert_equal(remote[0].device_id, PEER_ID);
    zassert_true(remote[0].data == CdmpStateData(Bytes{ 0x01, 0x02 }));
    zassert_true(fixture.Remote(TYPE_B).empty());
}

ZTEST(cdmp_state, test_newer_version_replaces_and_older_is_ignored) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 5, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 6, TYPE_A, { 0x02 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }));

    // A late resend of an earlier change.
    fixture.Receive(PEER_ID, 5, TYPE_A, { 0x01 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }));

    for(const uint8_t version : { 5, 6, 5 }) {
        auto acknowledgement = fixture.WaitForAcknowledgement();
        zassert_true(acknowledgement.has_value(), "Every notification is acknowledged");
        zassert_equal(acknowledgement->version, version);
    }
}

ZTEST(cdmp_state, test_version_comparison_wraps) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 250, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 3, TYPE_A, { 0x02 });

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "3 follows 250 once the counter wraps");

    fixture.Receive(PEER_ID, 200, TYPE_A, { 0x03 });

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "200 is more than half the range behind 3");
}

ZTEST(cdmp_state, test_duplicate_is_acknowledged_without_reporting_a_change) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    int calls = 0;
    fixture.state->RegisterStateHandler(TYPE_A, [&calls](uint8_t) {
        calls++;
    });
    fixture.DrainQueue();
    calls = 0;

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    fixture.DrainQueue();
    zassert_equal(calls, 1);

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    // The same version is the same change, whatever its data.
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x09 });
    fixture.DrainQueue();

    zassert_equal(calls, 1);
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }));
    for(int i = 0; i < 3; i++)
        zassert_true(fixture.WaitForAcknowledgement().has_value(), "Duplicates are acknowledged too");
}

ZTEST(cdmp_state, test_withdrawal_removes_the_copy) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 2, TYPE_A, {});

    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_true(fixture.WaitForAcknowledgement().has_value());
    auto acknowledgement = fixture.WaitForAcknowledgement();
    zassert_true(acknowledgement.has_value(), "A withdrawal is acknowledged");
    zassert_equal(acknowledgement->version, 2);

    fixture.Receive(PEER_ID, 4, TYPE_A, { 0x04 });
    fixture.Receive(PEER_ID, 3, TYPE_A, {});

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x04 }), "An older withdrawal does not remove a newer record");
}

ZTEST(cdmp_state, test_notifications_from_outside_the_device_table_are_ignored) {
    StateFixture fixture;
    fixture.GoOnline();

    // From a device that has not joined the table yet. The table never holds this unit's own ID, so
    // its own notification, which a loopback controller hands back, is ignored by the own-ID check
    // and by the table check alike; this covers the outcome, not which of the two checks fires.
    fixture.Receive(OWN_ID, 1, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });

    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_false(
        fixture.WaitForAcknowledgement(NO_FRAME_TIMEOUT_MS).has_value(),
        "The unknown sender resends until it has joined the table"
    );

    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }));
}

ZTEST(cdmp_state, test_notifications_are_ignored_while_offline) {
    StateFixture fixture;
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });

    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_false(fixture.WaitForAcknowledgement(NO_FRAME_TIMEOUT_MS).has_value());
}

ZTEST(cdmp_state, test_digest_covers_own_records_in_type_order) {
    const StateFixture fixture;
    zassert_equal(fixture.device->GetStateDigest(), 0, "No records, digest 0");

    // The parameters the spec names: polynomial 0x07, initial value 0, no reflection, no final XOR.
    const std::array<uint8_t, 9> check = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    zassert_equal(crc8_ccitt(0, check.data(), check.size()), 0xF4);

    // Set out of order: type B gets version 1, type A version 2.
    zassert_true(fixture.state->SetLocalState(TYPE_B, Bytes{ 0x07 }));
    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01, 0x02 }));

    zassert_equal(fixture.device->GetStateDigest(), Digest({ { TYPE_A, 2, { 0x01, 0x02 } }, { TYPE_B, 1, { 0x07 } } }));

    // The current value again is no change.
    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01, 0x02 }));
    zassert_equal(fixture.device->GetStateDigest(), Digest({ { TYPE_A, 2, { 0x01, 0x02 } }, { TYPE_B, 1, { 0x07 } } }));

    fixture.state->ClearLocalState(TYPE_A);
    zassert_equal(
        fixture.device->GetStateDigest(), Digest({ { TYPE_B, 1, { 0x07 } } }), "A withdrawn record leaves the digest"
    );

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x05 }));
    zassert_equal(
        fixture.device->GetStateDigest(), Digest({ { TYPE_A, 4, { 0x05 } }, { TYPE_B, 1, { 0x07 } } }),
        "The withdrawal took version 3"
    );

    fixture.state->ClearLocalState(TYPE_A);
    fixture.state->ClearLocalState(TYPE_B);
    zassert_equal(fixture.device->GetStateDigest(), 0);
}

ZTEST(cdmp_state, test_invalid_local_states_are_rejected) {
    const StateFixture fixture;

    zassert_false(fixture.state->SetLocalState(0, Bytes{ 0x01 }), "Type 0 is invalid");
    zassert_false(fixture.state->SetLocalState(TYPE_A, Bytes{ 1, 2, 3, 4, 5, 6 }), "At most 5 data bytes");
    zassert_equal(fixture.device->GetStateDigest(), 0);
}

ZTEST(cdmp_state, test_local_records_fill_up_and_withdrawals_give_way) {
    const StateFixture fixture;

    for(size_t i = 0; i < CONFIG_EERIE_LEAP_CDMP_STATE_MAX_LOCAL_RECORDS; i++)
        zassert_true(fixture.state->SetLocalState(TYPE_A + i, Bytes{ 0x01 }));

    zassert_false(fixture.state->SetLocalState(0xF0, Bytes{ 0x01 }), "Every slot holds a record");

    // Offline, the withdrawal waits for the next ONLINE, but a new type may take its slot.
    fixture.state->ClearLocalState(TYPE_A);

    zassert_true(fixture.state->SetLocalState(0xF0, Bytes{ 0x01 }));
}

ZTEST(cdmp_state, test_changing_own_record_sends_it_and_the_same_value_sends_nothing) {
    StateFixture fixture;
    fixture.GoOnline();

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));

    auto notification = fixture.WaitForNotification();
    zassert_true(notification.has_value());
    zassert_equal(notification->source_device_id, OWN_ID);
    zassert_equal(notification->version, 1);
    zassert_equal(notification->state_type, TYPE_A);
    zassert_true(notification->data == CdmpStateData(Bytes{ 0x01 }));

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_false(
        fixture.WaitForNotification(NO_FRAME_TIMEOUT_MS).has_value(), "No peer is online, so nothing is resent either"
    );

    fixture.state->ClearLocalState(TYPE_A);

    notification = fixture.WaitForNotification();
    zassert_true(notification.has_value());
    zassert_equal(notification->version, 2);
    zassert_true(notification->data.IsEmpty(), "No data withdraws the record");
}

ZTEST(cdmp_state, test_same_uid_claim_makes_the_copies_stale) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 9, TYPE_A, { 0x01 });

    // The peer restarted, and its version counter with it.
    fixture.ClaimAgain(PEER_ID, PEER_UID);

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }), "A stale copy still feeds its consumers");

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x02 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "A stale copy takes any version");

    fixture.Receive(PEER_ID, 0, TYPE_A, { 0x03 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "The applied record is fresh again");
}

ZTEST(cdmp_state, test_claim_with_another_uid_leaves_the_copies) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 9, TYPE_A, { 0x01 });

    fixture.ClaimAgain(PEER_ID, OTHER_PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x02 });

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }), "A conflicting claim is no restart");
}

ZTEST(cdmp_state, test_matching_digest_clears_stale_marks) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 5, TYPE_A, { 0x01 });
    fixture.ClaimAgain(PEER_ID, PEER_UID);

    fixture.Heartbeat(PEER_ID, Digest({ { TYPE_A, 5, { 0x01 } } }));

    zassert_false(fixture.WaitForSyncRequest(NO_FRAME_TIMEOUT_MS).has_value(), "The copies match");

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x02 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }), "Fresh again, so an older version is ignored");
}

// The mark is what a consumer of the copies cannot see otherwise: a record renewed with its old value
// raises no state handler, so only the mark tells that the unit restarted and is current again.
ZTEST(cdmp_state, test_remote_states_mark_stale_copies) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Receive(PEER_ID, 5, TYPE_A, { 0x01 });
    zassert_false(fixture.Remote(TYPE_A)[0].is_stale, "A record just received is current");

    fixture.ClaimAgain(PEER_ID, PEER_UID);
    auto remote = fixture.Remote(TYPE_A);
    zassert_equal(remote.size(), 1);
    zassert_true(remote[0].is_stale, "The restart marks it");
    zassert_true(remote[0].data == CdmpStateData(Bytes{ 0x01 }), "and leaves its value");

    fixture.Heartbeat(PEER_ID, Digest({ { TYPE_A, 5, { 0x01 } } }));
    zassert_false(fixture.Remote(TYPE_A)[0].is_stale, "A matching digest confirms the copy");

    fixture.ClaimAgain(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    remote = fixture.Remote(TYPE_A);
    zassert_false(remote[0].is_stale, "The same value sent again renews it");
    zassert_true(remote[0].data == CdmpStateData(Bytes{ 0x01 }));

    fixture.Heartbeat(PEER_ID, Digest({ { TYPE_A, 7, { 0x09 } } }));
    zassert_true(fixture.Remote(TYPE_A)[0].is_stale, "A digest that differs marks it");

    fixture.Receive(PEER_ID, 2, TYPE_A, { 0x02 });
    remote = fixture.Remote(TYPE_A);
    zassert_false(remote[0].is_stale, "and the next record renews it");
    zassert_true(remote[0].data == CdmpStateData(Bytes{ 0x02 }));
}

ZTEST(cdmp_state, test_digest_mismatch_marks_stale_and_requests_one_sync_per_interval) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 5, TYPE_A, { 0x01 });

    // The peer restarted unseen, and its new record was lost.
    const uint8_t peer_digest = Digest({ { TYPE_A, 1, { 0x02 } } });
    fixture.Heartbeat(PEER_ID, peer_digest);

    auto request = fixture.WaitForSyncRequest();
    zassert_true(request.has_value(), "A mismatch requests a replay");
    zassert_equal(request->target_device_id, PEER_ID);

    // Its replay.
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x02 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "The stale copy took the lower version");

    fixture.Heartbeat(PEER_ID, 0x00);

    zassert_false(
        fixture.WaitForSyncRequest(NO_FRAME_TIMEOUT_MS).has_value(),
        "At most one request per source and heartbeat interval"
    );

    fixture.Receive(PEER_ID, 0, TYPE_A, { 0x03 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x03 }), "The second mismatch still marks the copies stale");
}

ZTEST(cdmp_state, test_mismatch_without_copies_requests_sync) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.Heartbeat(PEER_ID, 0x00);
    zassert_false(fixture.WaitForSyncRequest(NO_FRAME_TIMEOUT_MS).has_value(), "No records on either side");

    fixture.Heartbeat(PEER_ID, Digest({ { TYPE_A, 1, { 0x01 } } }));

    auto request = fixture.WaitForSyncRequest();
    zassert_true(request.has_value(), "This unit missed every record of the peer");
    zassert_equal(request->target_device_id, PEER_ID);
}

ZTEST(cdmp_state, test_fresh_digest_drops_stale_copies_the_source_withdrew) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 2, TYPE_B, { 0x02 });

    int calls = 0;
    fixture.state->RegisterStateHandler(TYPE_B, [&calls](uint8_t) {
        calls++;
    });
    fixture.DrainQueue();
    calls = 0;

    // The peer withdrew type B, and this unit missed it.
    const uint8_t peer_digest = Digest({ { TYPE_A, 1, { 0x01 } } });
    fixture.Heartbeat(PEER_ID, peer_digest);
    zassert_true(fixture.WaitForSyncRequest().has_value());

    // The replay brings type A only.
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_B, { 0x02 }), "Kept until the next heartbeat");

    fixture.Heartbeat(PEER_ID, peer_digest);
    fixture.DrainQueue();

    zassert_true(fixture.Remote(TYPE_B).empty(), "The fresh copies match, so the stale one was withdrawn");
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x01 }));
    zassert_equal(calls, 1);
}

ZTEST(cdmp_state, test_restarted_source_without_records_drops_its_copies) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 7, TYPE_A, { 0x01 });
    fixture.ClaimAgain(PEER_ID, PEER_UID);

    // Back online before it has set any record.
    fixture.Heartbeat(PEER_ID, 0x00);

    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_false(fixture.WaitForSyncRequest(NO_FRAME_TIMEOUT_MS).has_value());
}

ZTEST(cdmp_state, test_departure_drops_the_source_copies) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.AddPeer(OTHER_PEER_ID, OTHER_PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    fixture.Receive(OTHER_PEER_ID, 1, TYPE_A, { 0x02 });

    int calls = 0;
    fixture.state->RegisterStateHandler(TYPE_A, [&calls](uint8_t) {
        calls++;
    });
    fixture.DrainQueue();
    calls = 0;

    fixture.network->RemoveDevice(PEER_ID);
    fixture.DrainQueue();

    const auto remote = fixture.Remote(TYPE_A);
    zassert_equal(remote.size(), 1);
    zassert_equal(remote[0].device_id, OTHER_PEER_ID);
    zassert_equal(calls, 1);
}

ZTEST(cdmp_state, test_leaving_online_drops_all_copies_stops_resending_and_keeps_own_records) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });

    int calls = 0;
    fixture.state->RegisterStateHandler(TYPE_A, [&calls](uint8_t) {
        calls++;
    });
    fixture.DrainQueue();
    calls = 0;

    // The peer does not acknowledge, so the change would be resent after 50 ms.
    zassert_true(fixture.state->SetLocalState(TYPE_B, Bytes{ 0x02 }));
    zassert_true(fixture.WaitForNotification().has_value());

    fixture.device->Reset();
    fixture.DrainQueue();

    zassert_false(fixture.WaitForNotification(RESEND_WINDOW_MS).has_value(), "Leaving ONLINE stops the resends");
    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_equal(calls, 1);
    zassert_equal(fixture.device->GetStateDigest(), Digest({ { TYPE_B, 1, { 0x02 } } }));

    fixture.GoOnline();

    auto notification = fixture.WaitForNotification();
    zassert_true(notification.has_value(), "Back online, the unit announces its records");
    zassert_equal(notification->version, 1, "Unchanged, so with its old version");
    zassert_equal(notification->state_type, TYPE_B);
}

ZTEST(cdmp_state, test_records_beyond_the_capacity_are_dropped_and_skip_the_digest_check) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    constexpr size_t CAPACITY = CONFIG_EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS;
    for(size_t i = 0; i <= CAPACITY; i++)
        fixture.Receive(PEER_ID, 1, static_cast<uint8_t>(TYPE_A + i), { 0x01 });

    zassert_true(
        fixture.HasRemote(PEER_ID, static_cast<uint8_t>(TYPE_A + CAPACITY - 1), { 0x01 }), "What fits is kept"
    );
    zassert_true(fixture.Remote(static_cast<uint8_t>(TYPE_A + CAPACITY)).empty());

    fixture.Heartbeat(PEER_ID, 0x00);
    zassert_false(
        fixture.WaitForSyncRequest(NO_FRAME_TIMEOUT_MS).has_value(),
        "Its copies are incomplete, so its digest is not checked"
    );

    // The peer withdraws a record, which makes room.
    fixture.Receive(PEER_ID, 2, TYPE_A, {});
    fixture.Heartbeat(PEER_ID, 0x00);

    zassert_true(fixture.WaitForSyncRequest().has_value(), "With room again, the digest is checked");
}

ZTEST(cdmp_state, test_state_handler_runs_on_registration_and_on_changes) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    std::vector<uint8_t> calls;
    bool is_on_work_queue = true;
    const int handler_id = fixture.state->RegisterStateHandler(TYPE_A, [&](uint8_t state_type) {
        is_on_work_queue = is_on_work_queue && fixture.work_queue->IsCurrentThread();
        calls.push_back(state_type);
    });
    fixture.DrainQueue();

    zassert_equal(calls.size(), 1, "Called once on registration");
    zassert_equal(calls[0], TYPE_A);
    zassert_true(is_on_work_queue);

    fixture.Receive(PEER_ID, 1, TYPE_A, { 0x01 });
    fixture.Receive(PEER_ID, 2, TYPE_B, { 0x01 });
    fixture.DrainQueue();

    zassert_equal(calls.size(), 2, "Only changes of its own type");

    fixture.state->UnregisterStateHandler(handler_id);
    fixture.Receive(PEER_ID, 3, TYPE_A, { 0x02 });
    fixture.DrainQueue();

    zassert_equal(calls.size(), 2);
}

ZTEST(cdmp_state, test_malformed_frames_are_dropped_without_a_response) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    fixture.state->ProcessStateChangeFrame(Bytes{ PEER_ID, 1 });
    fixture.state->ProcessStateChangeFrame(Bytes{ PEER_ID, 1, TYPE_A, 1, 2, 3, 4, 5, 6 });
    fixture.state->ProcessStateChangeFrame(Bytes{ PEER_ID, 1, 0x00, 0x01 });

    zassert_false(
        fixture.WaitForAcknowledgement(NO_FRAME_TIMEOUT_MS).has_value(),
        "Too short, too long or of type 0: not acknowledged"
    );
    zassert_true(fixture.Remote(TYPE_A).empty());
    zassert_true(fixture.Remote(0x00).empty());

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    auto notification = fixture.WaitForNotification();
    zassert_true(notification.has_value());

    // Without its result code.
    fixture.state->ProcessStateChangeResponseFrame(Bytes{ PEER_ID, OWN_ID, notification->version });

    auto resend = fixture.WaitForNotification();
    zassert_true(resend.has_value(), "A truncated acknowledgement is none");
    zassert_equal(resend->version, notification->version);
}

ZTEST(cdmp_state, test_departed_peer_leaves_the_acknowledgement_sets) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);
    fixture.AddPeer(OTHER_PEER_ID, OTHER_PEER_UID);

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    auto notification = fixture.WaitForNotification();
    zassert_true(notification.has_value());

    fixture.network->RemoveDevice(PEER_ID);
    fixture.Acknowledge(OTHER_PEER_ID, notification->version);

    zassert_false(
        fixture.WaitForNotification(RESEND_WINDOW_MS).has_value(),
        "The remaining peer acknowledged, and the one that left is not waited for"
    );
}

ZTEST(cdmp_state, test_sync_request_skips_a_withdrawal_still_being_resent) {
    StateFixture fixture;
    fixture.GoOnline();
    // It never acknowledges, so the withdrawal keeps its slot through its resends.
    fixture.AddPeer(PEER_ID, PEER_UID);

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_true(fixture.state->SetLocalState(TYPE_B, Bytes{ 0x02 }));
    fixture.state->ClearLocalState(TYPE_B);

    const auto result = fixture.state->ProcessSyncRequest(SyncRequest(OWN_ID));

    zassert_true(result.has_value(), "A unicast request is answered");
    zassert_equal(result->result_code, CdmpResultCode::SUCCESS);
    zassert_equal(result->data.size(), 1);
    zassert_equal(result->data[0], 1, "Only the record the unit holds is replayed, not the withdrawal");
}

ZTEST(cdmp_state, test_broadcast_sync_request_is_replayed_after_the_stagger) {
    StateFixture fixture;
    fixture.GoOnline(30);
    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_true(fixture.WaitForNotification().has_value());

    // Twenty devices with lower IDs put this unit 100 ms into the stagger.
    for(uint8_t device_id = 2; device_id < 22; device_id++)
        fixture.AddPeer(device_id, 0x01000000 + device_id);
    const auto stagger = static_cast<int64_t>(fixture.device->GetStaggeredMessageDelay());
    zassert_equal(stagger, 20 * CdmpConstants::STAGGERED_MESSAGE_TIME_OFFSET_MS);

    const int64_t requested_ms = k_uptime_get();
    zassert_false(
        fixture.state->ProcessSyncRequest(SyncRequest(CdmpDevice::DEVICE_ID_BROADCAST)).has_value(),
        "A broadcast request gets no response"
    );

    auto replay = fixture.WaitForNotification();
    const int64_t elapsed_ms = k_uptime_get() - requested_ms;

    zassert_true(replay.has_value(), "Every unit replays its records to a broadcast request");
    zassert_equal(replay->state_type, TYPE_A);
    zassert_true(
        elapsed_ms >= stagger - TIMING_TOLERANCE_BELOW_MS,
        "Replayed %lld ms after the request, before the %lld ms stagger", static_cast<long long>(elapsed_ms),
        static_cast<long long>(stagger)
    );
}

ZTEST(cdmp_state, test_stopped_service_sends_nothing_while_the_device_is_still_online) {
    StateFixture fixture;
    fixture.GoOnline();

    // CdmpService stops its services before the device leaves ONLINE.
    zassert_true(fixture.state->Stop());

    zassert_true(fixture.state->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_false(fixture.state->ProcessSyncRequest(SyncRequest(OWN_ID)).has_value(), "No answer once stopped");
    zassert_false(fixture.state->ProcessSyncRequest(SyncRequest(CdmpDevice::DEVICE_ID_BROADCAST)).has_value());

    zassert_false(
        fixture.WaitForNotification(NO_FRAME_TIMEOUT_MS).has_value(), "Neither the change nor a replay is sent"
    );

    zassert_true(fixture.state->Start());

    auto notification = fixture.WaitForNotification();
    zassert_true(notification.has_value(), "Started again while ONLINE, the unit announces its records");
    zassert_equal(notification->state_type, TYPE_A);
    zassert_equal(notification->version, 1);
    zassert_true(fixture.WaitForSyncRequest().has_value(), "and asks the others for theirs");
}

ZTEST(cdmp_state, test_record_left_behind_by_other_changes_is_stamped_again) {
    StateFixture fixture;
    fixture.GoOnline();

    // Each change is sent before the next one is made, as when they come slowly.
    const auto set = [&fixture](uint8_t state_type, uint8_t value) {
        zassert_true(fixture.state->SetLocalState(state_type, Bytes{ value }));
        fixture.DrainQueue();
    };

    set(TYPE_A, 0x01);
    // More changes of another type than a receiver's window of 127 versions spans.
    for(int i = 0; i < 130; i++)
        set(TYPE_B, static_cast<uint8_t>(i));
    set(TYPE_A, 0x02);

    std::vector<CdmpStateChangeMessage> notifications;
    while(auto notification = fixture.WaitForNotification(NO_FRAME_TIMEOUT_MS))
        notifications.push_back(notification.value());

    std::vector<uint8_t> versions_of_a;
    for(const auto& notification : notifications) {
        if(notification.state_type == TYPE_A)
            versions_of_a.push_back(notification.version);
    }

    zassert_true(versions_of_a.size() > 2, "TYPE_A was sent again, unchanged, while TYPE_B changed");
    for(size_t i = 1; i < versions_of_a.size(); i++) {
        zassert_true(
            static_cast<int8_t>(versions_of_a[i] - versions_of_a[i - 1]) > 0,
            "Version %d of TYPE_A does not compare as newer than %d", versions_of_a[i], versions_of_a[i - 1]
        );
    }

    // A receiver given the same notifications from a peer ends with both current values.
    fixture.AddPeer(PEER_ID, PEER_UID);
    for(const auto& notification : notifications)
        fixture.Receive(PEER_ID, notification.version, notification.state_type, ToBytes(notification.data));

    zassert_true(fixture.HasRemote(PEER_ID, TYPE_A, { 0x02 }), "The last change of TYPE_A was applied");
    zassert_true(fixture.HasRemote(PEER_ID, TYPE_B, { 129 }));
}

ZTEST(cdmp_state, test_next_heartbeat_requests_a_sync_again_even_if_early) {
    StateFixture fixture;
    fixture.GoOnline();
    fixture.AddPeer(PEER_ID, PEER_UID);

    const uint8_t peer_digest = Digest({ { TYPE_A, 1, { 0x01 } } });
    fixture.Heartbeat(PEER_ID, peer_digest);
    zassert_true(fixture.WaitForSyncRequest().has_value());

    // The replay was lost. The work queue was slower with the previous heartbeat than with this one,
    // so this one is handled a little less than an interval later.
    k_msleep(CdmpConstants::HEARTBEAT_INTERVAL_MS - 250);
    fixture.Heartbeat(PEER_ID, peer_digest);

    auto request = fixture.WaitForSyncRequest();
    zassert_true(request.has_value(), "Every heartbeat that does not match gets a request");
    zassert_equal(request->target_device_id, PEER_ID);
}

ZTEST_SUITE(cdmp_state_messages, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_state_messages, test_notification_round_trips) {
    const Bytes frame = NotificationFrame(3, 200, 0x42, { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E });

    zassert_equal(frame.size(), 8);
    zassert_equal(frame[0], 3);
    zassert_equal(frame[1], 200);
    zassert_equal(frame[2], 0x42);
    zassert_equal(frame[7], 0x0E);

    const auto message = CdmpStateChangeMessage::FromCanFrame(frame);
    zassert_equal(message.source_device_id, 3);
    zassert_equal(message.version, 200);
    zassert_equal(message.state_type, 0x42);
    zassert_true(message.data == CdmpStateData(Bytes{ 0x0A, 0x0B, 0x0C, 0x0D, 0x0E }));

    const auto withdrawal = CdmpStateChangeMessage::FromCanFrame(NotificationFrame(3, 201, 0x42, {}));
    zassert_true(withdrawal.data.IsEmpty(), "A 3-byte frame withdraws the record");
}

ZTEST(cdmp_state_messages, test_malformed_frames_are_rejected) {
    const auto is_rejected = [](auto parse) {
        try {
            parse();
        } catch(const std::invalid_argument&) {
            return true;
        }

        return false;
    };

    const Bytes short_notification = { 3, 1 };
    const Bytes long_notification = { 3, 1, 0x10, 1, 2, 3, 4, 5, 6 };
    const Bytes short_response = { 2, 3, 1 };

    zassert_true(is_rejected([&] {
        (void)CdmpStateChangeMessage::FromCanFrame(short_notification);
    }));
    zassert_true(is_rejected([&] {
        (void)CdmpStateChangeMessage::FromCanFrame(long_notification);
    }));
    zassert_true(is_rejected([&] {
        (void)CdmpStateChangeResponseMessage::FromCanFrame(short_response);
    }));
    zassert_true(is_rejected([] {
        (void)CdmpStateData(Bytes{ 1, 2, 3, 4, 5, 6 });
    }));

    // The receive path parses without exceptions.
    zassert_false(CdmpStateChangeMessage::TryFromCanFrame(short_notification).has_value());
    zassert_false(CdmpStateChangeMessage::TryFromCanFrame(long_notification).has_value());
    zassert_false(CdmpStateChangeResponseMessage::TryFromCanFrame(short_response).has_value());
    zassert_true(CdmpStateChangeMessage::TryFromCanFrame(NotificationFrame(3, 1, 0x10, {})).has_value());
}

ZTEST(cdmp_state_messages, test_response_round_trips) {
    const CdmpStateChangeResponseMessage sent{
        .responding_device_id = 2, .source_device_id = 3, .version = 77, .result_code = CdmpResultCode::SUCCESS
    };

    const auto frame = sent.ToCanFrame();
    zassert_equal(frame.size(), 4);
    zassert_equal(frame[0], 2);
    zassert_equal(frame[1], 3);
    zassert_equal(frame[2], 77);
    zassert_equal(frame[3], 0x00);

    const auto received = CdmpStateChangeResponseMessage::FromCanFrame(frame);
    zassert_equal(received.responding_device_id, 2);
    zassert_equal(received.source_device_id, 3);
    zassert_equal(received.version, 77);
    zassert_equal(received.result_code, CdmpResultCode::SUCCESS);
}
