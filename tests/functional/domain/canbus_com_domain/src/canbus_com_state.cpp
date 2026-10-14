#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_com_test_support.h"

using namespace canbus_com_test;

namespace {

constexpr CanbusComStateType LOGGING_STATUS = CanbusComStateType::LOGGING_STATUS;
constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(LOGGING_STATUS);
// A second application type, which the handlers of LOGGING_STATUS do not hear about.
constexpr uint8_t OTHER_STATE_TYPE = 0x7F;
constexpr auto OTHER_STATE = static_cast<CanbusComStateType>(OTHER_STATE_TYPE);

// A second unit next to the scripted peer, with an ID above the unit's.
constexpr uint8_t OTHER_PEER_ID = 3;
constexpr uint32_t OTHER_PEER_UID = 0x33333333;

// A state type as an application defines one: it encodes this unit's record and decodes another's.
class TestState : public ICanbusComState {
private:
    uint8_t value_ = 0;

public:
    explicit TestState(uint8_t value) : value_(value) {}
    explicit TestState(std::span<const uint8_t> data) : value_(data.empty() ? 0 : data[0]) {}

    [[nodiscard]] uint8_t GetValue() const { return value_; }

    [[nodiscard]] CanbusComStateType GetStateType() const override { return LOGGING_STATUS; }

    [[nodiscard]] CanbusComStateData GetData() const override {
        const std::array<uint8_t, 1> data = {value_};
        return CanbusComStateData(data);
    }
};

static_assert(CanbusComState<TestState>);

CanbusComStateData Data(const Bytes& bytes) {
    return CanbusComStateData(bytes);
}

using RemoteStateRecords = std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS>;

/**
 * @brief The unit's next @p notifications.size() notifications, ordered by state type: going online
 * sends every record at once, in an order the test leaves to the unit.
 */
void WaitForNotifications(Harness& harness, std::span<CdmpStateChangeMessage> notifications) {
    for(auto& notification : notifications) {
        auto next = harness.WaitForNotification();
        zassert_true(next.has_value(), "The unit sends each of its records");
        notification = *next;
    }

    std::ranges::sort(notifications, {}, &CdmpStateChangeMessage::state_type);
}

} // namespace

struct canbus_com_state_fixture {
    Harness* harness;
    // Outlives the harness, whose stop may still call the handler.
    HandlerProbe* probe;
};

static void* CanbusComStateSetup() {
    static canbus_com_state_fixture fixture{};

    return &fixture;
}

static void CanbusComStateBefore(void* data) {
    auto* fixture = static_cast<canbus_com_state_fixture*>(data);
    fixture->probe = new HandlerProbe();
    fixture->harness = new Harness();
}

static void CanbusComStateAfter(void* data) {
    auto* fixture = static_cast<canbus_com_state_fixture*>(data);

    if(fixture->probe != nullptr)
        fixture->probe->Unregister();

    delete fixture->harness;
    fixture->harness = nullptr;
    delete fixture->probe;
    fixture->probe = nullptr;

    ResetLoopbackDevice();
}

ZTEST_SUITE(canbus_com_state, NULL, CanbusComStateSetup, CanbusComStateBefore, CanbusComStateAfter, NULL);

ZTEST_F(canbus_com_state, test_state_set_before_start_is_announced_on_online) {
    Harness& harness = *fixture->harness;
    // Initialized but not started: the record waits for the first ONLINE.
    zassert_true(harness.Transport().SetLocalState(LOGGING_STATUS, Data({0x01, 0x00})));

    harness.BringOnline();

    auto notification = harness.WaitForNotification();
    zassert_true(notification.has_value(), "The unit announces its record on going online");
    zassert_equal(notification->source_device_id, OWN_ID);
    zassert_equal(notification->version, 1, "The first change of the unit is version 1");
    zassert_equal(notification->state_type, 0x10, "LOGGING_STATUS is state type 0x10 on the wire");
    zassert_true(notification->data == Data({0x01, 0x00}));
}

ZTEST_F(canbus_com_state, test_typed_state_is_shared_and_withdrawn) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    zassert_true(harness.Transport().SetLocalState(TestState(0x05)));

    auto change = harness.WaitForNotification();
    zassert_true(change.has_value(), "A typed state goes out as its type and data");
    zassert_equal(change->state_type, LOGGING_STATUS_CODE);
    zassert_equal(TestState(change->data.GetData()).GetValue(), 0x05, "and decodes from the data");
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, change->version));

    zassert_true(harness.Transport().SetLocalState(TestState(0x05)));
    zassert_false(harness.WaitForNotification(NO_FRAME_TIMEOUT_MS).has_value(),
        "Setting the current value again sends nothing");

    harness.Transport().ClearLocalState(LOGGING_STATUS);

    auto withdrawal = harness.WaitForNotification();
    zassert_true(withdrawal.has_value(), "Clearing the state withdraws the record");
    zassert_equal(withdrawal->state_type, LOGGING_STATUS_CODE);
    zassert_equal(withdrawal->version, change->version + 1);
    zassert_true(withdrawal->data.IsEmpty(), "with a notification without data");
}

ZTEST_F(canbus_com_state, test_handler_runs_on_registration_and_on_each_change) {
    Harness& harness = *fixture->harness;
    HandlerProbe& probe = *fixture->probe;

    // Before the start: the CDMP work queue runs from Initialize().
    probe.Register(harness.Transport(), LOGGING_STATUS);

    auto first = probe.WaitForCall();
    zassert_true(first.has_value(), "A new handler is called once, so that it catches up");
    zassert_true(first->empty(), "There are no records yet");
    zassert_false(probe.WaitForCall(NO_FRAME_TIMEOUT_MS).has_value(), "Only once");

    harness.BringOnline();
    harness.Settle();
    zassert_false(probe.WaitForCall(NO_FRAME_TIMEOUT_MS).has_value(), "Going online changes no record");

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, {0x01}));

    auto added = probe.WaitForCall();
    zassert_true(added.has_value(), "Called when another unit adds a record");
    zassert_equal(added->size(), 1, "The handler reads the record it is called for");
    zassert_equal(added->at(0).device_id, PEER_ID);
    zassert_true(added->at(0).data == Data({0x01}));

    // A duplicate, a new version of the same value, and a record of another type.
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, {0x01}));
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 2, LOGGING_STATUS_CODE, {0x01}));
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 3, OTHER_STATE_TYPE, {0x07}));
    zassert_true(harness.WaitForAcknowledgement(PEER_ID, 3), "The unit applied all three");
    zassert_false(probe.WaitForCall(NO_FRAME_TIMEOUT_MS).has_value(),
        "Not called when the data of its type stays the same");

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 4, LOGGING_STATUS_CODE, {0x00, 0x01}));

    auto changed = probe.WaitForCall();
    zassert_true(changed.has_value(), "Called when the record changes");
    zassert_equal(changed->size(), 1);
    zassert_true(changed->at(0).data == Data({0x00, 0x01}));

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 5, LOGGING_STATUS_CODE, {}));

    auto removed = probe.WaitForCall();
    zassert_true(removed.has_value(), "Called when the record is withdrawn");
    zassert_true(removed->empty());

    zassert_true(probe.CallsWereAsRegistered(), "Every call ran on the CDMP work queue, with its own type");

    probe.Unregister();
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 6, LOGGING_STATUS_CODE, {0x01}));
    zassert_true(harness.WaitForAcknowledgement(PEER_ID, 6));
    zassert_false(probe.WaitForCall(NO_FRAME_TIMEOUT_MS).has_value(), "Not called once unregistered");
}

ZTEST_F(canbus_com_state, test_remote_states_are_copied_in_device_order) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();
    harness.Join(OTHER_PEER_ID, OTHER_PEER_UID);

    // The higher ID first, so that the order comes from the IDs.
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(OTHER_PEER_ID, 1, LOGGING_STATUS_CODE, {0x03}));
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, OTHER_STATE_TYPE, {0x09}));
    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 2, LOGGING_STATUS_CODE, {0x01}));
    zassert_true(harness.WaitForAcknowledgement(PEER_ID, 2), "The unit applied all three");

    RemoteStateRecords records{};
    zassert_equal(harness.Transport().GetRemoteStates(LOGGING_STATUS, records), 2,
        "One record per unit, and none of another type");
    zassert_equal(records[0].device_id, PEER_ID, "Ordered by device ID");
    zassert_true(records[0].data == Data({0x01}));
    zassert_equal(records[1].device_id, OTHER_PEER_ID);
    zassert_true(records[1].data == Data({0x03}));

    std::array<RemoteStateRecord, 1> lowest{};
    zassert_equal(harness.Transport().GetRemoteStates(LOGGING_STATUS, lowest), 1, "As many as fit");
    zassert_equal(lowest[0].device_id, PEER_ID, "from the lowest ID");

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 3, LOGGING_STATUS_CODE, {0x00}));
    zassert_true(harness.WaitForAcknowledgement(PEER_ID, 3));
    zassert_true(records[0].data == Data({0x01}), "A copy keeps the values it was taken with");

    RemoteStateRecords later{};
    zassert_equal(harness.Transport().GetRemoteStates(LOGGING_STATUS, later), 2);
    zassert_true(later[0].data == Data({0x00}), "and the next copy has the change");
    zassert_true(later[1].data == Data({0x03}));
}

ZTEST_F(canbus_com_state, test_a_span_of_the_capacity_takes_every_unit) {
    Harness& harness = *fixture->harness;
    HandlerProbe& probe = *fixture->probe;
    harness.BringOnline();
    harness.Settle();

    // Five units share the type, and only the one with the highest ID reports 0x01, as when one of
    // several loggers logs.
    constexpr std::array<uint8_t, 4> joined_ids = {3, 4, 5, 6};
    constexpr uint8_t ACTIVE_ID = joined_ids.back();
    static_assert(INetworkStateTransport::MAX_REMOTE_RECORDS > joined_ids.size());

    for(const uint8_t device_id : joined_ids)
        harness.Join(device_id, 0x30000000 + device_id);

    probe.Register(harness.Transport(), LOGGING_STATUS);
    zassert_true(probe.WaitForCall().has_value());

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, {0x00}));
    for(const uint8_t device_id : joined_ids) {
        const uint8_t value = device_id == ACTIVE_ID ? 0x01 : 0x00;
        harness.SendAsPeer(STATE_CHANGE, NotificationFrame(device_id, 1, LOGGING_STATUS_CODE, {value}));
    }
    // The unit processes the notifications in order, so the last one acknowledged means all applied.
    zassert_true(harness.WaitForAcknowledgement(ACTIVE_ID, 1), "The unit applied all five");

    auto seen = probe.WaitForLastCall();
    zassert_true(seen.has_value(), "The handler hears of the records");
    zassert_equal(seen->size(), 1 + joined_ids.size(), "and reads every unit's record");
    zassert_equal(seen->back().device_id, ACTIVE_ID);
    zassert_true(seen->back().data == Data({0x01}), "including that of the unit with the highest ID");

    RemoteStateRecords records{};
    zassert_equal(harness.Transport().GetRemoteStates(LOGGING_STATUS, records), 1 + joined_ids.size());
    zassert_equal(records[joined_ids.size()].device_id, ACTIVE_ID);

    std::array<RemoteStateRecord, 4> short_span{};
    zassert_equal(harness.Transport().GetRemoteStates(LOGGING_STATUS, short_span), short_span.size(),
        "A shorter span is filled");
    zassert_true(std::ranges::none_of(short_span, [](const auto& record) { return record.data == Data({0x01}); }),
        "and misses the unit with the highest ID, which is why readers take the capacity");
}

ZTEST_F(canbus_com_state, test_local_states_survive_a_restart) {
    Harness& harness = *fixture->harness;
    // Two records before the first ONLINE: LOGGING_STATUS is version 1, the other type version 2.
    zassert_true(harness.Transport().SetLocalState(LOGGING_STATUS, Data({0x01})));
    zassert_true(harness.Transport().SetLocalState(OTHER_STATE, Data({0x0A})));
    harness.BringOnline();

    std::array<CdmpStateChangeMessage, 2> announced{};
    WaitForNotifications(harness, announced);
    zassert_equal(announced[0].state_type, LOGGING_STATUS_CODE);
    zassert_equal(announced[0].version, 1);
    zassert_equal(announced[1].state_type, OTHER_STATE_TYPE);
    zassert_equal(announced[1].version, 2);
    for(const auto& notification : announced)
        harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, notification.version));
    harness.Settle();

    // What CanbusService's reset and updated handlers do to the COM service on a CAN reconfiguration.
    zassert_true(harness.com->Stop());
    // Only the other type changes while stopped, as version 3; LOGGING_STATUS is left alone.
    zassert_true(harness.Transport().SetLocalState(OTHER_STATE, Data({0x0B})));
    harness.bus.Clear();

    harness.BringOnline();

    std::array<CdmpStateChangeMessage, 2> restarted{};
    WaitForNotifications(harness, restarted);
    zassert_equal(restarted[0].state_type, LOGGING_STATUS_CODE, "The record left alone outlives the stop");
    zassert_equal(restarted[0].version, 1, "as it was");
    zassert_true(restarted[0].data == Data({0x01}));
    zassert_equal(restarted[1].state_type, OTHER_STATE_TYPE, "The record changed while stopped is kept");
    zassert_equal(restarted[1].version, 3, "with its change");
    zassert_true(restarted[1].data == Data({0x0B}));
}

ZTEST_F(canbus_com_state, test_stop_drops_remote_states_and_keeps_handlers) {
    Harness& harness = *fixture->harness;
    HandlerProbe& probe = *fixture->probe;
    harness.BringOnline();
    harness.Settle();

    probe.Register(harness.Transport(), LOGGING_STATUS);
    zassert_true(probe.WaitForCall().has_value());

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, {0x01}));
    auto added = probe.WaitForCall();
    zassert_true(added.has_value());
    zassert_equal(added->size(), 1);

    zassert_true(harness.com->Stop());

    auto dropped = probe.WaitForCall();
    zassert_true(dropped.has_value(), "Stopping drops the other units' records, and says so");
    zassert_true(dropped->empty());

    harness.bus.Clear();
    harness.BringOnline();
    harness.Settle();

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, {0x01}));

    auto again = probe.WaitForCall();
    zassert_true(again.has_value(), "The handler outlives the restart");
    zassert_equal(again->size(), 1);
    zassert_equal(again->at(0).device_id, PEER_ID);
    zassert_true(probe.CallsWereAsRegistered());
}

namespace {

// Fills the controller's free filter slots with a filter no test frame matches, then frees them.
size_t CountFreeControllerFilters() {
    const can_filter unused{.id = 0x0F0, .mask = CAN_STD_ID_MASK, .flags = 0};
    std::vector<int> filter_ids;

    int filter_id = 0;
    while((filter_id = can_add_rx_filter(
        LoopbackDevice(), [](const device*, can_frame*, void*) {}, nullptr, &unused)) >= 0) {

        filter_ids.push_back(filter_id);
    }

    for(int id : filter_ids)
        can_remove_rx_filter(LoopbackDevice(), id);

    return filter_ids.size();
}

} // namespace

// The CDMP IDs share one filter at the default base, so an online unit with SMP bound leaves the
// rest of the controller's filters to the sensors' CAN routes.
ZTEST_F(canbus_com_state, test_online_unit_takes_two_controller_filters) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();
    zassert_not_equal(harness.com->GetAddress(), 0, "SMP is bound to the claimed ID");

    const size_t used = CONFIG_CAN_LOOPBACK_MAX_FILTERS - CountFreeControllerFilters();
    zassert_equal(used, 2, "CDMP and SMP take %zu controller filters, expected 2", used);
}
