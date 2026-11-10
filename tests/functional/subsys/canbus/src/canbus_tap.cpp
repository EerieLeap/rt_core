#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_test_support.h"

using namespace canbus_test;

using eerie_leap::subsys::canbus::CanFilter;
using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::canbus::ICanFrameTap;

namespace {

constexpr std::array<uint8_t, 3> PAYLOAD = { 0x01, 0x02, 0x03 };
constexpr uint32_t BLOCKER_ID = 0x7F0;
constexpr size_t MAX_FILTERS = CONFIG_CAN_LOOPBACK_MAX_FILTERS;

// Accepts every extended id addressed to 0x07 in bits 8..15.
constexpr uint32_t TARGET_MASK = 0x1FFFFF00;
constexpr CanFilter TARGET_FILTER = CanFilter::Masked(CanId::Extended(0x1FF00700), TARGET_MASK);

struct TapEvent {
    enum class Kind {
        FRAME,
        STATE,
        FILTER_SWAP
    };

    Kind kind = Kind::FRAME;
    CanFrame frame{};
    uint64_t time_us = 0;
    bool is_transmit = false;
    int tx_error = 0;
    can_state state = CAN_STATE_ERROR_ACTIVE;
    can_bus_err_cnt counters{};
    bool may_have_missed = false;
};

// Everything the tap hears, in order. Called from the receive thread, read from the test thread.
class RecordingTap : public ICanFrameTap {
private:
    k_mutex lock_{};
    std::vector<TapEvent> events_;

    void Push(const TapEvent& event) {
        k_mutex_lock(&lock_, K_FOREVER);
        events_.push_back(event);
        k_mutex_unlock(&lock_);
    }

public:
    RecordingTap() {
        k_mutex_init(&lock_);
    }

    void OnFrame(const CanFrame& frame, uint64_t time_us, bool is_transmit, int tx_error) override {
        Push(
            { .kind = TapEvent::Kind::FRAME,
                .frame = frame,
                .time_us = time_us,
                .is_transmit = is_transmit,
                .tx_error = tx_error }
        );
    }

    void OnState(can_state state, can_bus_err_cnt counters, uint64_t time_us) override {
        Push({ .kind = TapEvent::Kind::STATE, .time_us = time_us, .state = state, .counters = counters });
    }

    void OnFilterSwap(bool may_have_missed, uint64_t time_us) override {
        Push({ .kind = TapEvent::Kind::FILTER_SWAP, .time_us = time_us, .may_have_missed = may_have_missed });
    }

    std::vector<TapEvent> Events() {
        k_mutex_lock(&lock_, K_FOREVER);
        std::vector<TapEvent> copy = events_;
        k_mutex_unlock(&lock_);

        return copy;
    }

    size_t Count(TapEvent::Kind kind) {
        size_t count = 0;
        for(const TapEvent& event : Events()) {
            if(event.kind == kind)
                count++;
        }

        return count;
    }

    // Frames of one identifier, received (is_transmit false) or echoed (true).
    size_t CountFrames(CanId id, bool is_transmit) {
        size_t count = 0;
        for(const TapEvent& event : Events()) {
            if(event.kind == TapEvent::Kind::FRAME
                && event.frame.id == id.id
                && event.frame.is_extended == id.is_extended
                && event.is_transmit == is_transmit)
                count++;
        }

        return count;
    }

    TapEvent FirstFrame(CanId id, bool is_transmit) {
        for(const TapEvent& event : Events()) {
            if(event.kind == TapEvent::Kind::FRAME
                && event.frame.id == id.id
                && event.frame.is_extended == id.is_extended
                && event.is_transmit == is_transmit)
                return event;
        }

        zassert_unreachable("The tap did not hear 0x%x (transmit %d)", id.id, is_transmit);
        return {};
    }
};

// Waits a few milliseconds at a time, so native_sim's clock advances and the receive thread runs.
template<typename Predicate>
bool WaitUntil(Predicate predicate, int timeout_ms = 2000) {
    for(int waited = 0; waited <= timeout_ms; waited += 5) {
        if(predicate())
            return true;

        k_sleep(K_MSEC(5));
    }

    return predicate();
}

// Long enough for the loopback thread and the receive thread to deliver anything still in flight.
void Settle() {
    k_sleep(K_MSEC(100));
}

void NoopTxCallback(const device*, int, void*) {}
void NoopRxCallback(const device*, can_frame*, void*) {}

// Parks the loopback TX thread in a TX-complete callback, so queued frames stay queued until released.
k_sem tx_release;

void BlockingTxCallback(const device*, int, void* user_data) {
    k_sem_take(static_cast<k_sem*>(user_data), K_SECONDS(10));
}

bool BlockTxThread() {
    k_sem_reset(&tx_release);

    const can_frame frame = { .id = BLOCKER_ID, .dlc = 0, .flags = 0 };
    if(can_send(LoopbackDevice(), &frame, K_NO_WAIT, BlockingTxCallback, &tx_release) != 0)
        return false;

    // Let the TX thread take the frame and park in the callback.
    k_sleep(K_MSEC(20));

    return true;
}

// Takes `count` hardware filters away from the bus, to leave it less room than the tap needs.
std::vector<int> OccupyFilters(size_t count) {
    std::vector<int> ids;
    const can_filter any = { .id = 0, .mask = 0, .flags = 0 };

    for(size_t i = 0; i < count; i++) {
        const int id = can_add_rx_filter(LoopbackDevice(), NoopRxCallback, nullptr, &any);
        zassert_true(id >= 0, "Filter %zu of %zu was not granted [%d]", i, count, id);
        ids.push_back(id);
    }

    return ids;
}

void ReleaseFilters(const std::vector<int>& ids) {
    for(const int id : ids)
        can_remove_rx_filter(LoopbackDevice(), id);
}

// How many filters the controller still has room for.
size_t FreeFilters() {
    std::vector<int> ids;
    const can_filter any = { .id = 0, .mask = 0, .flags = 0 };

    for(size_t i = 0; i <= MAX_FILTERS; i++) {
        const int id = can_add_rx_filter(LoopbackDevice(), NoopRxCallback, nullptr, &any);
        if(id < 0)
            break;

        ids.push_back(id);
    }

    ReleaseFilters(ids);

    return ids.size();
}

// A tap that stays in OnFrame() until released, to show that clearing waits for a call in progress.
class BlockingTap : public ICanFrameTap {
public:
    k_sem entered{};
    k_sem release{};
    atomic_t frame_calls = ATOMIC_INIT(0);

    BlockingTap() {
        k_sem_init(&entered, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
    }

    void OnFrame(const CanFrame&, uint64_t, bool, int) override {
        atomic_inc(&frame_calls);
        k_sem_give(&entered);
        k_sem_take(&release, K_SECONDS(5));
    }

    void OnState(can_state, can_bus_err_cnt, uint64_t) override {}
    void OnFilterSwap(bool, uint64_t) override {}
};

K_THREAD_STACK_DEFINE(clearer_stack, 8192);
k_thread clearer_thread;
atomic_t clearer_done;

void ClearTapInThread(void* canbus, void*, void*) {
    static_cast<Canbus*>(canbus)->SetTap(nullptr);
    atomic_set(&clearer_done, 1);
}

// The frames a run of the same four sends gives the handlers, in the order they arrive.
std::vector<uint64_t> RunSequence(Canbus& canbus, FrameCollector& collector) {
    const size_t start = collector.Count();

    const std::array<uint8_t, 1> first = { 0xA1 };
    const std::array<uint8_t, 1> second = { 0xB2 };

    zassert_ok(canbus.SendFrame(CanId::Standard(0x100), first));
    zassert_ok(canbus.SendFrame(CanId::Standard(0x200), first)); // no handler
    zassert_ok(canbus.SendFrame(CanId::Extended(0x1FF00705), first));
    zassert_ok(canbus.SendFrame(CanId::Standard(0x100), second));

    for(int i = 0; i < 3; i++)
        zassert_true(collector.Wait(), "Handler call %d did not arrive", i);

    Settle();

    std::vector<uint64_t> calls;
    for(size_t i = start; i < collector.Count(); i++) {
        const CanFrame frame = collector.At(i);
        calls.push_back(
            frame.id | (static_cast<uint64_t>(frame.is_extended) << 32) | (static_cast<uint64_t>(frame.data[0]) << 40)
        );
    }

    return calls;
}

void* SetupSuite() {
    k_sem_init(&tx_release, 0, 1);

    return nullptr;
}

void AfterTest(void* fixture) {
    k_sem_give(&tx_release);
    ResetLoopbackDevice(fixture);
}

} // namespace

ZTEST_SUITE(canbus_tap, NULL, SetupSuite, NULL, AfterTest, NULL);

ZTEST(canbus_tap, test_receive_times_do_not_decrease_in_send_order) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    constexpr size_t FRAMES = 5;

    const uint64_t before = Canbus::NowUs();
    for(size_t i = 0; i < FRAMES; i++) {
        zassert_ok(canbus->SendFrame(CanId::Standard(0x100), PAYLOAD));
        zassert_true(collector.Wait());

        // native_sim's clock moves only while a thread sleeps.
        k_sleep(K_MSEC(20));
    }
    const uint64_t after = Canbus::NowUs();

    zassert_equal(collector.Count(), FRAMES);

    for(size_t i = 0; i < FRAMES; i++) {
        const uint64_t time_us = collector.At(i).time_us;

        zassert_true(time_us >= before && time_us <= after, "Frame %zu is stamped outside the run", i);

        if(i > 0)
            zassert_true(time_us >= collector.At(i - 1).time_us, "Frame %zu is stamped before its predecessor", i);
    }

    zassert_true(collector.At(FRAMES - 1).time_us > collector.At(0).time_us, "The times never advanced");
}

ZTEST(canbus_tap, test_tap_hears_each_received_frame_once_with_its_fields) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    // A frame a consumer's filter matches, one it doesn't, one extended.
    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    zassert_true(canbus->SetTap(&tap));

    const uint64_t before = Canbus::NowUs();
    zassert_ok(canbus->SendFrame(CanId::Standard(0x100), PAYLOAD));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x321), PAYLOAD));
    zassert_ok(canbus->SendFrame(CanId::Extended(0x18FF0001), PAYLOAD));
    zassert_true(collector.Wait());
    Settle();
    const uint64_t after = Canbus::NowUs();

    for(const CanId id : { CanId::Standard(0x100), CanId::Standard(0x321), CanId::Extended(0x18FF0001) }) {
        zassert_equal(tap.CountFrames(id, false), 1, "0x%x must reach the tap exactly once", id.id);

        const TapEvent event = tap.FirstFrame(id, false);
        zassert_false(event.frame.is_transmit);
        zassert_equal(event.frame.dlc, PAYLOAD.size());
        zassert_equal(event.frame.data.size(), PAYLOAD.size());
        zassert_equal(event.frame.data[2], 0x03);
        zassert_equal(event.time_us, event.frame.time_us, "The frame carries the time the tap is given");
        zassert_true(event.time_us >= before && event.time_us <= after);
        zassert_equal(event.tx_error, 0);
    }

    zassert_equal(collector.Count(), 1, "Only the matching frame reaches the consumer");

    zassert_true(canbus->SetTap(nullptr));
}

ZTEST(canbus_tap, test_handlers_get_the_same_frames_with_and_without_a_tap) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);
    zassert_true(canbus->RegisterFrameReceivedHandler(TARGET_FILTER, [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    const std::vector<uint64_t> baseline = RunSequence(*canbus, collector);
    zassert_equal(baseline.size(), 3);

    zassert_true(canbus->SetTap(&tap));
    const std::vector<uint64_t> tapped = RunSequence(*canbus, collector);

    // The tap heard all four frames of the run, the one without a handler included.
    zassert_equal(tap.CountFrames(CanId::Standard(0x100), false), 2);
    zassert_equal(tap.CountFrames(CanId::Standard(0x200), false), 1);
    zassert_equal(tap.CountFrames(CanId::Extended(0x1FF00705), false), 1);

    zassert_true(canbus->SetTap(nullptr));
    const size_t heard = tap.Events().size();

    const std::vector<uint64_t> untapped = RunSequence(*canbus, collector);

    zassert_true(tapped == baseline, "A tap changed what the handlers received");
    zassert_true(untapped == baseline, "Clearing the tap changed what the handlers receive");
    zassert_equal(tap.Events().size(), heard, "The tap heard a frame after it was cleared");
}

ZTEST(canbus_tap, test_tap_holds_the_two_accept_all_filters_instead_of_the_handlers) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [](const CanFrame&) {}) > 0);
    zassert_true(canbus->RegisterFrameReceivedHandler(TARGET_FILTER, [](const CanFrame&) {}) > 0);
    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Extended(0x123), [](const CanFrame&) {}) > 0);
    zassert_equal(FreeFilters(), MAX_FILTERS - 3);

    zassert_true(canbus->SetTap(&tap));
    zassert_equal(FreeFilters(), MAX_FILTERS - 2, "A tap takes two accept-all filters and the handlers none");

    zassert_true(canbus->SetTap(nullptr));
    zassert_equal(FreeFilters(), MAX_FILTERS - 3, "Clearing the tap gives the handlers their filters back");
}

ZTEST(canbus_tap, test_handler_registered_while_tapped_is_served_and_filtered_when_the_tap_goes) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->SetTap(&tap));

    const int id = canbus->RegisterFrameReceivedHandler(CanId::Standard(0x300), [&](const CanFrame& f) {
        collector.Collect(f);
    });
    zassert_true(id > 0);
    zassert_equal(FreeFilters(), MAX_FILTERS - 2, "A handler registered under a tap installs no filter");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x300), PAYLOAD));
    zassert_true(collector.Wait(), "The software dispatch must serve the handler");
    zassert_false(collector.Wait(200), "The handler must get the frame once");
    zassert_equal(tap.CountFrames(CanId::Standard(0x300), false), 1);

    zassert_true(canbus->SetTap(nullptr));
    zassert_equal(FreeFilters(), MAX_FILTERS - 1, "The handler's filter must be installed when the tap goes");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x300), PAYLOAD));
    zassert_true(collector.Wait(), "The handler must keep its frames after the tap");
    zassert_false(collector.Wait(200));
    zassert_equal(tap.CountFrames(CanId::Standard(0x300), false), 1, "The cleared tap heard a frame");

    zassert_true(canbus->RemoveFrameReceivedHandler(id));
}

ZTEST(canbus_tap, test_handler_removed_while_tapped_stays_removed_after_the_tap) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    const int id = canbus->RegisterFrameReceivedHandler(CanId::Standard(0x300), [&](const CanFrame& f) {
        collector.Collect(f);
    });
    zassert_true(id > 0);

    zassert_true(canbus->SetTap(&tap));
    zassert_true(canbus->RemoveFrameReceivedHandler(id));
    zassert_true(canbus->SetTap(nullptr));

    zassert_equal(FreeFilters(), MAX_FILTERS, "Nothing may stay installed");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x300), PAYLOAD));
    zassert_false(collector.Wait(200));
}

ZTEST(canbus_tap, test_filter_swap_is_announced_once_before_the_first_frame) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    const uint64_t before = Canbus::NowUs();
    zassert_true(canbus->SetTap(&tap));

    // With nothing queued, the receive thread announces the swap on its own.
    zassert_true(
        WaitUntil([&] {
            return tap.Count(TapEvent::Kind::FILTER_SWAP) == 1;
        }),
        "No OnFilterSwap()"
    );

    const TapEvent swap = tap.Events().at(0);
    zassert_equal(swap.kind, TapEvent::Kind::FILTER_SWAP, "The swap must come first");
    zassert_false(swap.may_have_missed, "The loopback has room for both sets of filters");
    zassert_true(swap.time_us >= before && swap.time_us <= Canbus::NowUs());

    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), false) == 1;
    }));
    Settle();

    zassert_equal(tap.Count(TapEvent::Kind::FILTER_SWAP), 1, "A swap is announced once");

    zassert_true(canbus->SetTap(nullptr));
    Settle();
    zassert_equal(tap.Count(TapEvent::Kind::FILTER_SWAP), 1, "Clearing the tap is not announced to it");
}

ZTEST(canbus_tap, test_filter_swap_precedes_a_frame_that_is_already_queued) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->SetTap(&tap));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), false) == 1;
    }));

    zassert_equal(tap.Events().at(0).kind, TapEvent::Kind::FILTER_SWAP, "The swap must come before any frame");
}

ZTEST(canbus_tap, test_swap_without_room_for_both_sets_reports_missed_frames_and_recovers) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    // One filter short of the room two accept-all filters need on top of the handlers'.
    const size_t handlers = MAX_FILTERS - 1;
    std::vector<int> ids;
    for(size_t i = 0; i < handlers; i++) {
        const int id = canbus->RegisterFrameReceivedHandler(
            CanId::Standard(static_cast<uint32_t>(0x400 + i)), [&](const CanFrame& f) {
                collector.Collect(f);
            }
        );
        zassert_true(id > 0);
        ids.push_back(id);
    }
    zassert_equal(FreeFilters(), 1);

    zassert_true(canbus->SetTap(&tap), "The swap must fall back to removing first");
    zassert_true(WaitUntil([&] {
        return tap.Count(TapEvent::Kind::FILTER_SWAP) == 1;
    }));
    zassert_true(tap.Events().at(0).may_have_missed, "Removing first may lose frames");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x400), PAYLOAD));
    zassert_true(collector.Wait());
    zassert_equal(tap.CountFrames(CanId::Standard(0x400), false), 1);

    // Back out: the handlers' filters and the tap's do not fit together, so the tap's go first.
    zassert_true(canbus->SetTap(nullptr));
    zassert_equal(FreeFilters(), MAX_FILTERS - handlers, "Every handler needs its filter back");

    const CanId last = CanId::Standard(static_cast<uint32_t>(0x400 + handlers - 1));
    zassert_ok(canbus->SendFrame(last, PAYLOAD));
    zassert_true(collector.Wait(), "The last handler lost its filter");

    for(const int id : ids)
        zassert_true(canbus->RemoveFrameReceivedHandler(id));
}

ZTEST(canbus_tap, test_swap_that_the_driver_rejects_leaves_everything_as_before) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    // The handler holds one filter and nothing is free: removing it makes room for one of the tap's two.
    std::vector<int> occupied = OccupyFilters(MAX_FILTERS - 1);

    zassert_false(canbus->SetTap(&tap), "The driver has no room for the tap's filters");
    zassert_equal(FreeFilters(), 0, "The handler's filter must be back and the tap's gone");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x100), PAYLOAD));
    zassert_true(collector.Wait(), "The handler must keep its hardware filter");
    Settle();
    zassert_equal(tap.Events().size(), 0, "A tap that was refused must hear nothing");

    ReleaseFilters(occupied);
    zassert_true(canbus->SetTap(&tap), "The tap must be settable once there is room");
    zassert_true(canbus->SetTap(nullptr));
}

ZTEST(canbus_tap, test_remote_frames_reach_the_tap_and_no_handler) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    // A remote frame asks for a length and carries no payload. SendFrame() cannot build one.
    const can_frame remote = { .id = 0x100, .dlc = 4, .flags = CAN_FRAME_RTR };

    zassert_ok(can_send(LoopbackDevice(), &remote, K_NO_WAIT, NoopTxCallback, nullptr));
    Settle();
    zassert_equal(collector.Count(), 0, "A handler must not get a remote frame, tap or not");

    zassert_true(canbus->SetTap(&tap));
    zassert_ok(can_send(LoopbackDevice(), &remote, K_NO_WAIT, NoopTxCallback, nullptr));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x100), false) == 1;
    }));
    Settle();

    const TapEvent event = tap.FirstFrame(CanId::Standard(0x100), false);
    zassert_true(event.frame.is_remote_request);
    zassert_equal(event.frame.dlc, 4, "A remote frame keeps the length it asks for");
    zassert_true(event.frame.data.empty());
    zassert_equal(collector.Count(), 0, "A handler must not get a remote frame, tap or not");

    // An ordinary frame behind it is dispatched as ever.
    zassert_ok(canbus->SendFrame(CanId::Standard(0x100), PAYLOAD));
    zassert_true(collector.Wait());
}

ZTEST(canbus_tap, test_each_send_is_echoed_to_the_tap_at_its_completion) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_ok(canbus->SendFrame(CanId::Standard(0x222), PAYLOAD));
    Settle();
    zassert_equal(tap.Events().size(), 0);

    zassert_true(canbus->SetTap(&tap));

    const uint64_t before = Canbus::NowUs();
    zassert_ok(canbus->SendFrame(CanId::Standard(0x222), PAYLOAD));
    zassert_ok(canbus->SendFrame(CanId::Extended(0x18FF0002), PAYLOAD));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x222), true) == 1
            && tap.CountFrames(CanId::Standard(0x222), false) == 1
            && tap.CountFrames(CanId::Extended(0x18FF0002), true) == 1
            && tap.CountFrames(CanId::Extended(0x18FF0002), false) == 1;
    }));
    Settle();
    const uint64_t after = Canbus::NowUs();

    // The loopback also loops each frame back as a received one; the echo is the unit's own copy.
    zassert_equal(tap.CountFrames(CanId::Standard(0x222), true), 1);
    zassert_equal(tap.CountFrames(CanId::Standard(0x222), false), 1);
    zassert_equal(tap.CountFrames(CanId::Extended(0x18FF0002), true), 1);
    zassert_equal(tap.CountFrames(CanId::Extended(0x18FF0002), false), 1);

    const TapEvent echo = tap.FirstFrame(CanId::Standard(0x222), true);
    zassert_true(echo.frame.is_transmit);
    zassert_equal(echo.tx_error, 0);
    zassert_equal(echo.frame.data.size(), PAYLOAD.size());
    zassert_equal(echo.frame.data[1], 0x02);
    zassert_equal(echo.frame.dlc, PAYLOAD.size());
    zassert_equal(echo.time_us, echo.frame.time_us);
    zassert_true(echo.time_us >= before && echo.time_us <= after);
    zassert_equal(echo.frame.is_can_fd, false);

    zassert_true(canbus->SetTap(nullptr));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x222), PAYLOAD));
    Settle();
    zassert_equal(tap.CountFrames(CanId::Standard(0x222), true), 1, "No echo once the tap is cleared");
    zassert_equal(canbus->GetTxEchoLostCount(), 0);
}

ZTEST(canbus_tap, test_echo_of_a_can_fd_frame_keeps_its_format) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus(CanbusType::CANFD, 500000);
    std::array<uint8_t, 12> payload{};
    payload[11] = 0x5A;

    zassert_true(canbus->SetTap(&tap));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), payload));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), true) == 1;
    }));

    const TapEvent echo = tap.FirstFrame(CanId::Standard(0x123), true);
    zassert_true(echo.frame.is_can_fd);
    zassert_true(echo.frame.is_bitrate_switch);
    zassert_equal(echo.frame.dlc, 9, "12 bytes is DLC 9");
    zassert_equal(echo.frame.data.size(), 12);
    zassert_equal(echo.frame.data[11], 0x5A);
}

ZTEST(canbus_tap, test_exhausted_tx_slots_still_send_and_count_the_lost_echoes) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    constexpr size_t BURST = Canbus::TX_SLOTS + 2;
    const CanId id = CanId::Standard(0x7F2);

    zassert_true(canbus->SetTap(&tap));
    zassert_true(BlockTxThread());

    // The loopback TX thread is parked, so no completion frees a slot while the burst goes out.
    for(size_t i = 0; i < BURST; i++)
        zassert_ok(canbus->SendFrame(id, PAYLOAD, K_NO_WAIT), "Send %zu must succeed with or without a slot", i);

    zassert_equal(canbus->GetTxEchoLostCount(), BURST - Canbus::TX_SLOTS);

    k_sem_give(&tx_release);

    zassert_true(
        WaitUntil([&] {
            return tap.CountFrames(id, false) == BURST;
        }),
        "Every frame must still be sent"
    );
    Settle();

    zassert_equal(tap.CountFrames(id, false), BURST);
    zassert_equal(tap.CountFrames(id, true), Canbus::TX_SLOTS, "One echo for each slot, none for the rest");
    zassert_equal(canbus->GetTxEchoLostCount(), BURST - Canbus::TX_SLOTS);

    // Completed frames gave their slots back.
    for(size_t i = 0; i < Canbus::TX_SLOTS; i++)
        zassert_ok(canbus->SendFrame(id, PAYLOAD, K_NO_WAIT));

    zassert_true(WaitUntil([&] {
        return tap.CountFrames(id, true) == 2 * Canbus::TX_SLOTS;
    }));
    zassert_equal(canbus->GetTxEchoLostCount(), BURST - Canbus::TX_SLOTS, "No further echo was lost");
}

ZTEST(canbus_tap, test_sends_without_a_tap_use_no_slots_and_lose_no_echo) {
    auto canbus = MakeRunningCanbus();

    zassert_true(BlockTxThread());

    for(size_t i = 0; i < Canbus::TX_SLOTS + 2; i++)
        zassert_ok(canbus->SendFrame(CanId::Standard(0x7F2), PAYLOAD, K_NO_WAIT));

    zassert_equal(canbus->GetTxEchoLostCount(), 0);

    k_sem_give(&tx_release);
    Settle();
}

ZTEST(canbus_tap, test_slots_of_a_stopped_bus_are_free_again) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();
    const CanId id = CanId::Standard(0x7F2);

    zassert_true(canbus->SetTap(&tap));
    zassert_true(BlockTxThread());

    for(size_t i = 0; i < Canbus::TX_SLOTS; i++)
        zassert_ok(canbus->SendFrame(id, PAYLOAD, K_NO_WAIT));

    // The loopback drops its queued frames on stop without completing them, as a driver may.
    zassert_true(canbus->Stop());
    k_sem_give(&tx_release);
    Settle();

    zassert_true(canbus->Configure(MakeConfig()));
    zassert_true(canbus->Start());
    zassert_true(canbus->SetTap(&tap));

    zassert_ok(canbus->SendFrame(id, PAYLOAD, K_NO_WAIT));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(id, true) >= 1;
    }));
    zassert_equal(canbus->GetTxEchoLostCount(), 0, "The stop must have freed the slots");
}

ZTEST(canbus_tap, test_state_changes_reach_the_tap_in_order_with_frames) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    // No tap: the entry is dropped by the receive thread and must not turn up later.
    canbus->OnBusStateChanged(CAN_STATE_ERROR_WARNING, { .tx_err_cnt = 100, .rx_err_cnt = 0 });
    Settle();

    zassert_true(canbus->SetTap(&tap));
    zassert_true(WaitUntil([&] {
        return tap.Count(TapEvent::Kind::FILTER_SWAP) == 1;
    }));
    zassert_equal(tap.Count(TapEvent::Kind::STATE), 0);

    const uint64_t before = Canbus::NowUs();
    canbus->OnBusStateChanged(CAN_STATE_ERROR_PASSIVE, { .tx_err_cnt = 130, .rx_err_cnt = 7 });
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));

    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), false) == 1;
    }));
    Settle();
    const uint64_t after = Canbus::NowUs();

    zassert_equal(tap.Count(TapEvent::Kind::STATE), 1);

    const std::vector<TapEvent> events = tap.Events();
    zassert_equal(events.at(1).kind, TapEvent::Kind::STATE, "The state change was queued before the frame");
    zassert_equal(events.at(1).state, CAN_STATE_ERROR_PASSIVE);
    zassert_equal(events.at(1).counters.tx_err_cnt, 130);
    zassert_equal(events.at(1).counters.rx_err_cnt, 7);
    zassert_true(events.at(1).time_us >= before && events.at(1).time_us <= after);
}

ZTEST(canbus_tap, test_set_tap_on_an_uninitialized_bus_fails) {
    RecordingTap tap;
    Canbus canbus(MakeConfig());

    zassert_false(canbus.SetTap(&tap));
    zassert_true(canbus.SetTap(nullptr), "There is no tap to clear");

    zassert_true(canbus.Initialize());
    zassert_true(canbus.Start());
    zassert_true(canbus.SetTap(&tap));
    zassert_true(canbus.Stop());

    zassert_false(canbus.SetTap(&tap), "A stopped bus is not initialized any more");
}

ZTEST(canbus_tap, test_a_bus_takes_one_tap_at_a_time) {
    RecordingTap first;
    RecordingTap second;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->SetTap(&first));
    zassert_true(canbus->SetTap(&first), "Setting the same tap again changes nothing");
    zassert_false(canbus->SetTap(&second), "Another tap must be refused");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(WaitUntil([&] {
        return first.CountFrames(CanId::Standard(0x123), false) == 1;
    }));
    Settle();
    zassert_equal(second.Events().size(), 0);

    zassert_true(canbus->SetTap(nullptr));
    zassert_true(canbus->SetTap(nullptr), "Clearing twice is fine");
    zassert_true(canbus->SetTap(&second));
}

ZTEST(canbus_tap, test_tap_is_not_called_after_it_is_cleared) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->SetTap(&tap));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), false) == 1;
    }));

    zassert_true(canbus->SetTap(nullptr));
    const size_t heard = tap.Events().size();

    canbus->OnBusStateChanged(CAN_STATE_BUS_OFF, { .tx_err_cnt = 255, .rx_err_cnt = 0 });
    for(int i = 0; i < 4; i++)
        zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));

    Settle();
    zassert_equal(tap.Events().size(), heard, "The tap was called after SetTap(nullptr) returned");
}

ZTEST(canbus_tap, test_clearing_the_tap_waits_for_a_call_in_progress) {
    BlockingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->SetTap(&tap));
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_ok(k_sem_take(&tap.entered, K_SECONDS(2)), "The tap was not called");

    atomic_set(&clearer_done, 0);
    k_thread_create(
        &clearer_thread, clearer_stack, K_THREAD_STACK_SIZEOF(clearer_stack), ClearTapInThread, canbus.get(), nullptr,
        nullptr, K_PRIO_PREEMPT(5), 0, K_NO_WAIT
    );

    k_sleep(K_MSEC(50));
    zassert_equal(atomic_get(&clearer_done), 0, "SetTap(nullptr) returned inside a call to the tap");

    k_sem_give(&tap.release);
    zassert_ok(k_thread_join(&clearer_thread, K_SECONDS(2)));
    zassert_equal(atomic_get(&clearer_done), 1);

    Settle();
    zassert_equal(atomic_get(&tap.frame_calls), 1, "The frames queued behind the call must not reach the tap");
}

ZTEST(canbus_tap, test_stopping_the_bus_clears_the_tap_and_releases_its_filters) {
    RecordingTap tap;
    auto canbus = MakeRunningCanbus();

    zassert_true(canbus->SetTap(&tap));
    zassert_true(canbus->Stop());
    zassert_equal(FreeFilters(), MAX_FILTERS, "The tap's filters must not outlive a stopped bus");

    const size_t heard = tap.Events().size();

    zassert_true(canbus->Configure(MakeConfig()));
    zassert_true(canbus->Start());
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    Settle();
    zassert_equal(tap.Events().size(), heard, "A restarted bus must not call the old tap");

    RecordingTap other;
    zassert_true(canbus->SetTap(&other), "The old tap must be gone");
}

ZTEST(canbus_tap, test_configure_clears_the_tap) {
    RecordingTap tap;
    auto canbus = std::make_unique<Canbus>(MakeConfig());

    zassert_true(canbus->Initialize());
    zassert_true(canbus->SetTap(&tap));

    // Initialize() left the controller started, which the loopback refuses to reconfigure.
    zassert_ok(can_stop(LoopbackDevice()));
    zassert_true(canbus->Configure(MakeConfig()));
    zassert_equal(FreeFilters(), MAX_FILTERS, "The tap's filters must go with the old configuration");
    zassert_true(canbus->Start());

    const size_t heard = tap.Events().size();
    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    Settle();
    zassert_equal(tap.Events().size(), heard);

    RecordingTap other;
    zassert_true(canbus->SetTap(&other), "Configure() must have removed the tap");
}

ZTEST(canbus_tap, test_destroying_a_tapped_bus_leaves_no_filter_behind) {
    RecordingTap tap;

    {
        auto canbus = MakeRunningCanbus();

        zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x100), [](const CanFrame&) {}) > 0);
        zassert_true(canbus->SetTap(&tap));
    }

    zassert_equal(FreeFilters(), MAX_FILTERS, "The driver would call back into a destroyed bus");
}

ZTEST(canbus_tap, test_tap_set_while_the_bitrate_is_detected_is_fed_once_it_is) {
    RecordingTap tap;
    // Bitrate 0 selects auto detection, which needs bus activity to succeed.
    auto canbus = std::make_unique<Canbus>(MakeConfig(CanbusType::CLASSICAL_CAN, 0));

    zassert_true(canbus->Initialize());
    zassert_true(canbus->Start());
    zassert_false(canbus->IsBitrateDetected());

    FrameCollector collector;
    zassert_true(canbus->RegisterFrameReceivedHandler(CanId::Standard(0x123), [&](const CanFrame& f) {
        collector.Collect(f);
    }) > 0);

    zassert_true(canbus->SetTap(&tap), "A tap may be set while the bitrate is detected");
    Settle();
    zassert_equal(tap.Events().size(), 0, "Nothing is installed yet, and no swap happened");

    // The loopback bus is silent; the detection needs a few frames in its sampling window.
    const can_frame activity = { .id = 0x7E0, .dlc = 0, .flags = 0 };
    for(int i = 0; i < 400 && !canbus->IsBitrateDetected(); i++) {
        can_send(LoopbackDevice(), &activity, K_NO_WAIT, NoopTxCallback, nullptr);
        k_sleep(K_MSEC(20));
    }
    zassert_true(canbus->IsBitrateDetected(), "The bitrate was not detected");

    zassert_true(
        WaitUntil([&] {
            return tap.Count(TapEvent::Kind::STATE) == 1;
        }),
        "No state after the detection"
    );

    const TapEvent state = tap.Events().at(0);
    zassert_equal(state.kind, TapEvent::Kind::STATE);
    zassert_equal(state.state, CAN_STATE_ERROR_ACTIVE);
    zassert_equal(state.counters.tx_err_cnt, 0);
    zassert_equal(tap.Count(TapEvent::Kind::FILTER_SWAP), 0, "The detection installs the filters, not a swap");

    // The tap's filters are in place and the handler is served by the software dispatch.
    zassert_equal(FreeFilters(), MAX_FILTERS - 2);

    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(collector.Wait());
    zassert_true(WaitUntil([&] {
        return tap.CountFrames(CanId::Standard(0x123), false) == 1
            && tap.CountFrames(CanId::Standard(0x123), true) == 1;
    }));

    zassert_true(canbus->SetTap(nullptr));
    zassert_equal(FreeFilters(), MAX_FILTERS - 1, "The handler's filter must replace the tap's");
}

ZTEST(canbus_tap, test_tap_set_and_cleared_while_the_bitrate_is_detected) {
    RecordingTap tap;
    auto canbus = std::make_unique<Canbus>(MakeConfig(CanbusType::CLASSICAL_CAN, 0));

    zassert_true(canbus->Initialize());
    zassert_true(canbus->Start());

    zassert_true(canbus->SetTap(&tap));
    zassert_true(canbus->SetTap(nullptr));
    zassert_true(canbus->SetTap(&tap));
    zassert_true(canbus->Stop());

    Settle();
    zassert_equal(tap.Events().size(), 0);
}
