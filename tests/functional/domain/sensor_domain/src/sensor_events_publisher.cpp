#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor_limits.h"
#include "subsys/event_bus/event_channel.h"

#include "sensor_events_test_support.h"

using namespace sensor_events_test_support;

using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;

namespace {

// The publisher stack the throwing filter case needs. With a throwing filter the publisher thread
// peaked at 1.5 KiB on qemu_cortex_a9, 5.9 on qemu_malta, 6.4 on qemu_cortex_a53 and 7.8 on
// qemu_riscv64, so 8192 would leave about 236 bytes there.
constexpr int THROWING_FILTER_STACK_SIZE = 12288;

// Stack the publisher thread must still have unused once a throw has been unwound. On every QEMU
// board but qemu_riscv64 (which has a guard below the stack) only the stack sentinel checks the
// stack, and it misses a frame that jumps past it, so a pass alone does not show that the stack
// was enough.
[[maybe_unused]] constexpr size_t PUBLISHER_STACK_MARGIN = 512;

// A frame of [a, b, c, d, sentinel] and a started publisher over it, stopped before anything goes.
// The sensor ids are the case's own: a failed case ends without running its destructors, and the
// bindings it leaves behind must not reach the sensors of the cases after it.
struct Rig {
    std::shared_ptr<Sensor> a;
    std::shared_ptr<Sensor> b;
    std::shared_ptr<Sensor> c;
    std::shared_ptr<Sensor> d;
    std::shared_ptr<Sensor> sentinel;
    std::vector<std::shared_ptr<Sensor>> sensors;

    std::shared_ptr<SensorReadingsFrame> frame = std::make_shared<SensorReadingsFrame>();
    std::unique_ptr<SensorEventsPublisher> publisher;

    explicit Rig(std::string_view name) :
        a(MakeSensor(std::string(name) + "_a")),
        b(MakeSensor(std::string(name) + "_b")),
        c(MakeSensor(std::string(name) + "_c")),
        d(MakeSensor(std::string(name) + "_d")),
        sentinel(MakeSensor(std::string(name) + "_sentinel")),
        sensors{ a, b, c, d, sentinel } {

        frame->Configure(sensors);

        publisher = std::make_unique<SensorEventsPublisher>(frame);
        zassert_true(publisher->Initialize());
        zassert_true(publisher->Start());
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    // A case may already have destroyed the publisher.
    ~Rig() {
        if(publisher != nullptr)
            publisher->Stop();
    }

    bool Round(std::initializer_list<Commit> commits, Recorder& sentinel_recorder) {
        return sensor_events_test_support::Round(*publisher, *frame, commits, *sentinel, sentinel_recorder);
    }
};

// A failure bound only, as WAIT_MS: a failed case ends without releasing its stall, and the bus
// worker must come free for the cases after it.
constexpr int STALL_LIMIT_MS = 5 * WAIT_MS;

// Two publisher ticks.
void LetTicksPass() {
    k_sleep(K_MSEC(2 * CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS));
}

// What a widget does to the bus worker while it waits for the LVGL lock: the first event its handler
// gets blocks the worker until Release(), and the events behind it wait on the channel.
class Stall {
private:
    k_sem entered_{};
    k_sem released_{};
    std::atomic<size_t> entry_count_{ 0 };

public:
    Stall() {
        k_sem_init(&entered_, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&released_, 0, K_SEM_MAX_LIMIT);
    }

    Stall(const Stall&) = delete;
    Stall& operator=(const Stall&) = delete;
    ~Stall() = default;

    // The body of the handler, on the bus thread.
    void OnEvent() {
        const size_t entry = entry_count_.fetch_add(1);

        k_sem_give(&entered_);

        if(entry == 0)
            k_sem_take(&released_, K_MSEC(STALL_LIMIT_MS));
    }

    bool WaitForEntry() {
        return k_sem_take(&entered_, K_MSEC(WAIT_MS)) == 0;
    }

    // The events its handler got, the held one included.
    size_t GetEntryCount() const {
        return entry_count_.load();
    }

    void Release() {
        k_sem_give(&released_);
    }
};

// The stall takes the given sensors only: a failed case leaves its subscription behind, and one
// without a filter would answer Accepts() for the sensors of the cases after it. Subscribe it
// before the recorders, so it holds each event before they get it.
AnySubscription SubscribeStall(const std::shared_ptr<Stall>& stall, std::vector<uint32_t> sensor_id_hashes) {
    std::ranges::sort(sensor_id_hashes);

    return CreateScopedSubscription(
        SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated,
        [hashes = std::move(sensor_id_hashes)](const SensorEventsChannel::EventMessage& event) {
            return std::ranges::binary_search(
                hashes, std::get<uint32_t>(event.payload.at(SensorPayloadType::SensorId))
            );
        },
        [stall](const SensorEventsChannel::EventMessage&) {
            stall->OnEvent();
        }
    );
}

// A Rig whose bus worker is stalled on a's first event. The first tick published a, b and the
// sentinel: a sits in the stalled handler and the others wait on the channel, so every later tick
// finds events there. c and d are committed too, but nobody accepts them.
struct StalledRig : Rig {
    std::shared_ptr<Stall> stall = std::make_shared<Stall>();
    std::shared_ptr<Recorder> a_recorder = std::make_shared<Recorder>();
    std::shared_ptr<Recorder> b_recorder = std::make_shared<Recorder>();
    std::shared_ptr<Recorder> sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription stall_subscription;
    AnySubscription a_binding;
    AnySubscription b_binding;
    AnySubscription sentinel_binding;
    size_t queued = 0; // The events waiting behind the stalled one.

    explicit StalledRig(std::string_view name) :
        Rig(name) {

        stall_subscription = SubscribeStall(stall, { a->id_hash });
        a_binding = Bind(*a, a_recorder);
        b_binding = Bind(*b, b_recorder);
        sentinel_binding = Bind(*sentinel, sentinel_recorder);
        zassert_not_null(stall_subscription.get());
        zassert_not_null(a_binding.get());
        zassert_not_null(b_binding.get());
        zassert_not_null(sentinel_binding.get());

        // One tick, as Round() makes it.
        zassert_true(publisher->Pause());
        CommitAll(1.0F);
        zassert_true(publisher->Resume());
        zassert_true(stall->WaitForEntry());

        // This thread outranks the publisher, so it wakes before the tick has published b and the
        // sentinel; the ticks that pass let it finish.
        LetTicksPass();
        queued = SensorEventsChannel::GetInstance().GetQueuedCount();
        zassert_true(queued > 0, "nothing waits behind the stalled event");
    }

    // One value for every sensor of the rig.
    void CommitAll(float value) {
        for(const auto& sensor : sensors)
            frame->AddOrUpdateReading(MakeReading(*sensor, value));
    }
};

// The two values a subscriber got through a stall: the first tick's and the one the tick after it carried.
void ExpectDeliveries(const Recorder& recorder, float first, float second, const char* name) {
    zassert_equal(recorder.Count(), 2, "%s", name);
    zassert_equal(recorder.At(0), first, "%s", name);
    zassert_equal(recorder.At(1), second, "%s", name);
}

void* Setup() {
    RegisterSensorEvents();

    return nullptr;
}

} // namespace

ZTEST_SUITE(sensor_events_publisher, NULL, Setup, NULL, NULL, NULL);

ZTEST(sensor_events_publisher, test_a_publisher_without_a_frame_does_not_start) {
    // On the heap, as in Rig: the object holds MAX_COUNT readings.
    auto publisher = std::make_unique<SensorEventsPublisher>(nullptr);

    zassert_false(publisher->Initialize());
    zassert_false(publisher->Start());
    zassert_true(publisher->IsStopped());

    // Nothing to mark.
    publisher->Replay();
}

ZTEST(sensor_events_publisher, test_a_reading_nobody_accepts_is_taken_but_not_published) {
    Rig rig("nobody");

    struct FilterCalls {
        std::atomic<int> a{ 0 };
        std::atomic<int> sentinel{ 0 };
    };

    const auto calls = std::make_shared<FilterCalls>();
    const auto sentinel_recorder = std::make_shared<Recorder>();

    // The only subscriber on the channel, so Accepts() reaches its filter for every sensor; with an
    // earlier subscriber that accepts the sentinel, any_of would stop before it.
    const uint32_t a_hash = rig.a->id_hash;
    const uint32_t sentinel_hash = rig.sentinel->id_hash;
    AnySubscription subscription = CreateScopedSubscription(
        SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated,
        [calls, a_hash, sentinel_hash](const SensorEventsChannel::EventMessage& event) {
            const auto id = std::get<uint32_t>(event.payload.at(SensorPayloadType::SensorId));

            if(id == a_hash)
                calls->a++;
            else if(id == sentinel_hash)
                calls->sentinel++;

            return id == sentinel_hash;
        },
        [sentinel_recorder](const SensorEventsChannel::EventMessage& event) {
            sentinel_recorder->Record(std::get<float>(event.payload.at(SensorPayloadType::Value)));
        }
    );
    zassert_not_null(subscription.get());

    for(int round = 0; round < 3; round++)
        zassert_true(rig.Round({ { rig.a.get(), 1.0F } }, *sentinel_recorder), "round %d", round);

    // One Accepts() for a, then cached and never dispatched; one Accepts() for the sentinel, then
    // one dispatch per round.
    zassert_equal(calls->a.load(), 1);
    zassert_equal(calls->sentinel.load(), 1 + 3);
    zassert_equal(sentinel_recorder->Count(), 3);

    // a was taken all the same.
    rig.publisher->Stop();
    std::array<SensorReading, 8> taken;
    zassert_equal(rig.frame->TakeProcessedReadings(taken), 0);
}

ZTEST(sensor_events_publisher, test_a_selector_filtered_subscriber_receives_only_its_sensor) {
    Rig rig("selector");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto b_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    AnySubscription b_binding = Bind(*rig.b, b_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);
    zassert_not_null(a_binding.get());
    zassert_not_null(b_binding.get());
    zassert_not_null(sentinel_binding.get());

    // c is bound by nobody.
    zassert_true(
        rig.Round({ { rig.a.get(), 1.0F }, { rig.b.get(), 2.0F }, { rig.c.get(), 3.0F } }, *sentinel_recorder)
    );

    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_recorder->At(0), 1.0F);
    zassert_equal(b_recorder->Count(), 1);
    zassert_equal(b_recorder->At(0), 2.0F);
}

ZTEST(sensor_events_publisher, test_a_subscription_added_after_the_cache_was_filled_is_honoured_at_the_next_tick) {
    Rig rig("late");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    // Nobody wants a yet, so the tick caches it as rejected.
    zassert_true(rig.Round({ { rig.a.get(), 1.0F } }, *sentinel_recorder));

    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    zassert_not_null(a_binding.get());

    zassert_true(rig.Round({ { rig.a.get(), 2.0F } }, *sentinel_recorder));

    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_recorder->At(0), 2.0F);
}

ZTEST(sensor_events_publisher, test_a_sensor_whose_last_binding_goes_is_asked_again_and_no_longer_published) {
    Rig rig("dropped");

    const auto a_calls = std::make_shared<std::atomic<int>>(0);
    const auto a_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();

    // Takes only the sentinel and counts how often it is asked about a. Subscribed before a's
    // binding, so Accepts() reaches it for a before any_of stops at the binding.
    const uint32_t a_hash = rig.a->id_hash;
    const uint32_t sentinel_hash = rig.sentinel->id_hash;
    AnySubscription counting = CreateScopedSubscription(
        SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated,
        [a_calls, a_hash, sentinel_hash](const SensorEventsChannel::EventMessage& event) {
            const auto id = std::get<uint32_t>(event.payload.at(SensorPayloadType::SensorId));

            if(id == a_hash)
                a_calls->fetch_add(1);

            return id == sentinel_hash;
        },
        [sentinel_recorder](const SensorEventsChannel::EventMessage& event) {
            sentinel_recorder->Record(std::get<float>(event.payload.at(SensorPayloadType::Value)));
        }
    );
    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    zassert_not_null(counting.get());
    zassert_not_null(a_binding.get());

    // a is cached as accepted: asked once by Accepts() and once more by the dispatch.
    zassert_true(rig.Round({ { rig.a.get(), 1.0F } }, *sentinel_recorder));
    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_calls->load(), 2);

    a_binding.reset();

    zassert_true(rig.Round({ { rig.a.get(), 2.0F } }, *sentinel_recorder));
    zassert_true(rig.Round({ { rig.a.get(), 3.0F } }, *sentinel_recorder));

    // One new Accepts() after the generation moved, cached as a rejection, and no dispatch.
    zassert_equal(a_calls->load(), 2 + 1);
    zassert_equal(a_recorder->Count(), 1);
}

ZTEST(sensor_events_publisher, test_values_committed_across_ticks_arrive_in_order) {
    Rig rig("order");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    zassert_true(rig.Round({ { rig.a.get(), 0.0F } }, *sentinel_recorder));
    zassert_true(rig.Round({ { rig.a.get(), 1.0F } }, *sentinel_recorder));
    zassert_true(rig.Round({ { rig.a.get(), 0.0F } }, *sentinel_recorder));

    // A value is not suppressed when it returns to an earlier one.
    zassert_equal(a_recorder->Count(), 3);
    zassert_equal(a_recorder->At(0), 0.0F);
    zassert_equal(a_recorder->At(1), 1.0F);
    zassert_equal(a_recorder->At(2), 0.0F);
}

ZTEST(sensor_events_publisher, test_Replay_publishes_the_latest_value_of_every_accepted_sensor_once) {
    Rig rig("replay");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto b_recorder = std::make_shared<Recorder>();
    const auto d_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    AnySubscription b_binding = Bind(*rig.b, b_recorder);
    AnySubscription d_binding = Bind(*rig.d, d_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    // Within one tick a sensor goes out once, with its latest value; d is never committed.
    zassert_true(rig.Round(
        { { rig.a.get(), 1.0F }, { rig.a.get(), 2.0F }, { rig.b.get(), 5.0F }, { rig.c.get(), 7.0F } },
        *sentinel_recorder
    ));
    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_recorder->At(0), 2.0F);

    a_recorder->Clear();
    b_recorder->Clear();

    // Replay marks the sentinel too, so its delivery ends the replayed tick.
    zassert_true(rig.publisher->Pause());
    rig.publisher->Replay();
    zassert_true(rig.publisher->Resume());
    zassert_true(sentinel_recorder->WaitForDelivery());

    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_recorder->At(0), 2.0F);
    zassert_equal(b_recorder->Count(), 1);
    zassert_equal(b_recorder->At(0), 5.0F);
    zassert_equal(d_recorder->Count(), 0, "a sensor without a processed reading has nothing to replay");

    // The replay ran once.
    zassert_true(rig.Round({}, *sentinel_recorder));
    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(b_recorder->Count(), 1);
}

ZTEST(sensor_events_publisher, test_Replay_reaches_a_sensor_bound_after_it_was_cached_as_rejected) {
    Rig rig("replay_late");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    // a is cached as rejected, and then nothing commits it again: only the replay can bring it.
    zassert_true(rig.Round({ { rig.a.get(), 3.0F } }, *sentinel_recorder));

    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    zassert_not_null(a_binding.get());

    zassert_true(rig.publisher->Pause());
    rig.publisher->Replay();
    zassert_true(rig.publisher->Resume());
    zassert_true(sentinel_recorder->WaitForDelivery());

    zassert_equal(a_recorder->Count(), 1);
    zassert_equal(a_recorder->At(0), 3.0F);

    zassert_true(rig.Round({}, *sentinel_recorder));
    zassert_equal(a_recorder->Count(), 1, "the replayed value went out once");
}

ZTEST(sensor_events_publisher, test_readings_without_a_value_are_not_published) {
    Rig rig("no_value");

    const auto a_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription a_binding = Bind(*rig.a, a_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    zassert_true(rig.Round({ { rig.a.get(), std::nullopt } }, *sentinel_recorder));

    zassert_equal(a_recorder->Count(), 0);

    rig.publisher->Stop();
    std::array<SensorReading, 8> taken;
    zassert_equal(rig.frame->TakeProcessedReadings(taken), 0, "the reading was taken all the same");
}

ZTEST(sensor_events_publisher, test_a_filter_that_throws_rejects_its_sensor_and_spares_the_rest) {
    // Unwinding the throw needs more stack than a tick (see the _STACK_SIZE help); native_sim does
    // not enforce thread stacks, so elsewhere the case runs only with a raised publisher stack.
#ifdef RT_CORE_TEST_RAISED_PUBLISHER_STACK
    // The raised_publisher_stack scenario exists to run this case: a build that does not raise the
    // stack, or does not fill the stacks for the measurement below, fails here instead of skipping.
    static_assert(
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_STACK_SIZE >= THROWING_FILTER_STACK_SIZE,
        "the raised_publisher_stack scenario must raise the publisher stack"
    );
    static_assert(
        IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO),
        "the raised_publisher_stack scenario measures the stack the unwind leaves unused"
    );
#else
    if(!IS_ENABLED(CONFIG_ARCH_POSIX)
        && CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_STACK_SIZE < THROWING_FILTER_STACK_SIZE)
        ztest_test_skip();
#endif

    Rig rig("throws");

    const auto throws = std::make_shared<std::atomic<int>>(0);
    const auto least_unused = std::make_shared<std::atomic<size_t>>(SIZE_MAX);
    const auto thrower = std::make_shared<std::atomic<k_tid_t>>(nullptr);
    const auto b_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();

    const uint32_t a_hash = rig.a->id_hash;
    AnySubscription throwing = CreateScopedSubscription(
        SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated,
        [throws, least_unused, thrower, a_hash](const SensorEventsChannel::EventMessage& event) {
            if(std::get<uint32_t>(event.payload.at(SensorPayloadType::SensorId)) != a_hash) {
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
                // On the thread that threw, the publisher's, after a's throw in the same tick: the
                // bus thread runs the filters too, on its own stack. The unused space is the
                // stack's high-water mark, so it covers the unwind.
                size_t unused = 0;
                if(k_current_get() == thrower->load() && k_thread_stack_space_get(k_current_get(), &unused) == 0) {
                    size_t seen = least_unused->load();
                    while(unused < seen && !least_unused->compare_exchange_weak(seen, unused)) {
                    }
                }
#endif
                return false;
            }

            throws->fetch_add(1);
            thrower->store(k_current_get());
            throw std::runtime_error("filter failed");
        },
        [](const SensorEventsChannel::EventMessage&) {}
    );
    AnySubscription b_binding = Bind(*rig.b, b_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    // a's slot comes before b's: the throw must not cost b or the sentinel their events.
    zassert_true(rig.Round({ { rig.a.get(), 1.0F }, { rig.b.get(), 2.0F } }, *sentinel_recorder));
    zassert_true(rig.Round({ { rig.a.get(), 3.0F }, { rig.b.get(), 4.0F } }, *sentinel_recorder));

    zassert_equal(b_recorder->Count(), 2);
    zassert_equal(b_recorder->At(0), 2.0F);
    zassert_equal(b_recorder->At(1), 4.0F);

    // Cached as a rejection: asked once, until the subscriptions change.
    zassert_equal(throws->load(), 1);

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
    // The unwind stayed inside the publisher's stack, with room to spare. Where the stacks are not
    // filled with a pattern (native_sim, which does not enforce them either) there is nothing to
    // measure.
    zassert_not_equal(least_unused->load(), SIZE_MAX, "the publisher's stack was not measured");
    TC_PRINT(
        "publisher stack: %zu of %d bytes unused after the unwind\n", least_unused->load(),
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_STACK_SIZE
    );
    zassert_true(least_unused->load() >= PUBLISHER_STACK_MARGIN, "the publisher stack is nearly used up by the unwind");
#endif
}

ZTEST(sensor_events_publisher, test_a_paused_publisher_publishes_nothing_until_resumed) {
    Rig rig("paused");

    const auto a_recorder = std::make_shared<Recorder>();
    AnySubscription a_binding = Bind(*rig.a, a_recorder);

    zassert_true(rig.publisher->Pause());
    rig.frame->AddOrUpdateReading(MakeReading(*rig.a, 4.0F));

    k_sleep(K_MSEC(5 * CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS));
    zassert_equal(a_recorder->Count(), 0);

    zassert_true(rig.publisher->Resume());
    zassert_true(a_recorder->WaitForDelivery());
    zassert_equal(a_recorder->At(0), 4.0F);
}

ZTEST(sensor_events_publisher, test_a_frame_larger_than_a_take_is_drained_within_two_ticks) {
    // One sensor more than a take returns, with the sentinel last, so it goes out only with a second
    // take: in the same tick if the bus has dispatched the first batch by then, else in the next,
    // as the publisher takes nothing while sensor_events holds events. Every sensor is accepted, so
    // each round also fills the acceptance cache and clears it when the next key finds it full. The
    // k_count events of a tick fit sensor_events' 128-event queue.
    static constexpr size_t K_COUNT = SensorLimits::MAX_COUNT + 1;
    static constexpr size_t SENTINEL = K_COUNT - 1;

    struct Deliveries {
        std::array<uint32_t, K_COUNT> sorted_hashes{};
        std::array<std::atomic<int>, K_COUNT> counts{}; // By sensor.
        Recorder sentinel;
    };

    const auto deliveries = std::make_shared<Deliveries>();

    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(K_COUNT);
    for(size_t i = 0; i < K_COUNT; i++) {
        sensors.push_back(MakeSensor("drain_" + std::to_string(i)));
        deliveries->sorted_hashes[i] = sensors.back()->id_hash;
    }

    std::ranges::sort(deliveries->sorted_hashes);

    const auto frame = std::make_shared<SensorReadingsFrame>();
    frame->Configure(sensors);

    // Takes this case's sensors only: a failed case leaves its subscription behind, and one without
    // a filter would answer Accepts() ahead of the counting filters of the cases after it.
    AnySubscription subscription = CreateScopedSubscription(
        SensorEventsChannel::GetInstance(), SensorEventType::DataUpdated,
        [deliveries](const SensorEventsChannel::EventMessage& event) {
            const auto id = std::get<uint32_t>(event.payload.at(SensorPayloadType::SensorId));

            return std::ranges::binary_search(deliveries->sorted_hashes, id);
        },
        [deliveries](const SensorEventsChannel::EventMessage& event) {
            // Sensor i commits i.
            const auto index = static_cast<size_t>(std::get<float>(event.payload.at(SensorPayloadType::Value)));
            if(index >= K_COUNT)
                return;

            deliveries->counts[index].fetch_add(1);

            if(index == SENTINEL)
                deliveries->sentinel.Record(static_cast<float>(index));
        }
    );
    zassert_not_null(subscription.get());

    auto publisher = std::make_unique<SensorEventsPublisher>(frame);
    zassert_true(publisher->Initialize());
    zassert_true(publisher->Start());

    for(int round = 1; round <= 2; round++) {
        zassert_true(publisher->Pause());

        for(size_t i = 0; i < K_COUNT; i++)
            frame->AddOrUpdateReading(MakeReading(*sensors[i], static_cast<float>(i)));

        const int64_t resumed_at = k_uptime_get();
        zassert_true(publisher->Resume());
        zassert_true(deliveries->sentinel.WaitForDelivery(), "round %d", round);
        const int64_t elapsed_ms = k_uptime_get() - resumed_at;

        for(size_t i = 0; i < K_COUNT; i++)
            zassert_equal(deliveries->counts[i].load(), round, "sensor %zu, round %d", i, round);

        // native_sim's clock moves only while every thread waits, so each tick after the first would
        // show as a whole period; QEMU's clock also runs while a tick works.
        if(IS_ENABLED(CONFIG_ARCH_POSIX)) {
            zassert_true(
                elapsed_ms < int64_t{ 2 } * CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS,
                "%lld ms from Resume() to the sentinel, round %d", static_cast<long long>(elapsed_ms), round
            );
        }
    }

    publisher->Stop();
}

ZTEST(sensor_events_publisher, test_ticks_publish_nothing_while_sensor_events_holds_events_then_the_newest_values) {
    StalledRig rig("stalled");
    auto& channel = SensorEventsChannel::GetInstance();

    // Each tick finds the first one's events waiting, so what is committed meanwhile stays in the
    // frame and nothing joins the queue.
    for(int value = 2; value <= 4; value++) {
        rig.CommitAll(static_cast<float>(value));
        LetTicksPass();

        zassert_equal(channel.GetQueuedCount(), rig.queued, "after committing %d", value);
    }

    zassert_equal(rig.stall->GetEntryCount(), 1);

    // Released, the events of the first tick come out and the next tick carries the newest value of
    // each sensor, none of the values in between.
    rig.stall->Release();
    zassert_true(rig.sentinel_recorder->WaitForDelivery());
    zassert_true(rig.sentinel_recorder->WaitForDelivery());

    ExpectDeliveries(*rig.a_recorder, 1.0F, 4.0F, "a");
    ExpectDeliveries(*rig.b_recorder, 1.0F, 4.0F, "b");
    ExpectDeliveries(*rig.sentinel_recorder, 1.0F, 4.0F, "sentinel");

    // Nothing is left in the frame for another tick.
    LetTicksPass();
    zassert_equal(rig.a_recorder->Count(), 2);
}

ZTEST(sensor_events_publisher, test_a_Replay_requested_while_ticks_are_skipped_happens_once_the_channel_drains) {
    StalledRig rig("replay_stalled");
    auto& channel = SensorEventsChannel::GetInstance();

    // c was committed by the first tick and cached as rejected. A subscriber bound since, as a
    // logging trigger is, is the one that asks for the replay.
    const auto c_recorder = std::make_shared<Recorder>();
    AnySubscription c_binding = Bind(*rig.c, c_recorder);
    zassert_not_null(c_binding.get());

    rig.publisher->Replay();
    LetTicksPass();

    zassert_equal(channel.GetQueuedCount(), rig.queued, "the replay did not wait for the channel to drain");

    rig.stall->Release();
    zassert_true(rig.sentinel_recorder->WaitForDelivery());
    zassert_true(rig.sentinel_recorder->WaitForDelivery());

    // Nothing was committed since the first tick, so a second delivery is the replay.
    ExpectDeliveries(*rig.a_recorder, 1.0F, 1.0F, "a");
    ExpectDeliveries(*rig.b_recorder, 1.0F, 1.0F, "b");
    ExpectDeliveries(*rig.sentinel_recorder, 1.0F, 1.0F, "sentinel");
    zassert_equal(c_recorder->Count(), 1);
    zassert_equal(c_recorder->At(0), 1.0F);

    // The replay went out once.
    LetTicksPass();
    zassert_equal(rig.a_recorder->Count(), 2);
    zassert_equal(c_recorder->Count(), 1);
}

ZTEST(sensor_events_publisher, test_readings_never_push_an_event_queued_before_them_out_of_sensor_events) {
    // One sensor more than a take returns, all accepted. A take queues MAX_COUNT events against the
    // 128 that sensor_events holds, so readings left to queue behind a stalled bus would overflow it
    // in the third tick, and it sheds the oldest event first.
    static constexpr size_t K_COUNT = SensorLimits::MAX_COUNT + 1;
    static constexpr int K_STALLED_TICKS = 4;
    static constexpr float PRESS = 7.0F;

    auto& channel = SensorEventsChannel::GetInstance();

    std::vector<std::shared_ptr<Sensor>> sensors;
    std::vector<uint32_t> sensor_id_hashes;
    sensors.reserve(K_COUNT);
    sensor_id_hashes.reserve(K_COUNT);
    for(size_t i = 0; i < K_COUNT; i++) {
        sensors.push_back(MakeSensor("flood_" + std::to_string(i)));
        sensor_id_hashes.push_back(sensors.back()->id_hash);
    }

    const auto frame = std::make_shared<SensorReadingsFrame>();
    frame->Configure(sensors);

    const auto commit_all = [&sensors, &frame](float value) {
        for(const auto& sensor : sensors)
            frame->AddOrUpdateReading(MakeReading(*sensor, value));
    };

    // The last sensor of the frame ends each tick. The press is no reading: a sensor outside the
    // frame, published by hand.
    const auto press = MakeSensor("flood_press");
    const auto stall = std::make_shared<Stall>();
    const auto press_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription stall_subscription = SubscribeStall(stall, sensor_id_hashes);
    AnySubscription press_binding = Bind(*press, press_recorder);
    AnySubscription sentinel_binding = Bind(*sensors.back(), sentinel_recorder);
    zassert_not_null(stall_subscription.get());
    zassert_not_null(press_binding.get());
    zassert_not_null(sentinel_binding.get());

    auto publisher = std::make_unique<SensorEventsPublisher>(frame);
    zassert_true(publisher->Initialize());
    zassert_true(publisher->Start());

    commit_all(1.0F);
    zassert_true(stall->WaitForEntry());
    LetTicksPass();

    const SensorEventsChannel::EventMessage press_event{ .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = { { SensorPayloadType::SensorId, press->id_hash }, { SensorPayloadType::Value, PRESS } } };
    channel.PublishAsync(press_event);

    // The tick published one take, MAX_COUNT sensors, and took no second one while those waited: one
    // event is in the handler, the others and the press are queued.
    const size_t queued = channel.GetQueuedCount();
    zassert_equal(queued, SensorLimits::MAX_COUNT);

    for(int tick = 0; tick < K_STALLED_TICKS; tick++) {
        commit_all(2.0F + static_cast<float>(tick));
        LetTicksPass();
    }

    zassert_equal(channel.GetQueuedCount(), queued, "readings were queued behind the press");
    zassert_equal(stall->GetEntryCount(), 1);

    // Released, the press arrives, then the newest value of every sensor: one take, and the last
    // sensor once the channel has drained again.
    stall->Release();
    zassert_true(sentinel_recorder->WaitForDelivery());

    zassert_equal(press_recorder->Count(), 1, "the press was pushed out");
    zassert_equal(press_recorder->At(0), PRESS);
    zassert_equal(stall->GetEntryCount(), 2 * SensorLimits::MAX_COUNT + 1);

    publisher->Stop();
}

ZTEST(sensor_events_publisher, test_a_running_publisher_can_be_destroyed) {
    const auto a_recorder = std::make_shared<Recorder>();

    {
        Rig rig("destroyed");
        AnySubscription a_binding = Bind(*rig.a, a_recorder);

        rig.frame->AddOrUpdateReading(MakeReading(*rig.a, 1.0F));
        zassert_true(a_recorder->WaitForDelivery());

        // Running, and without Stop(): the destructor cancels the task before the members it uses go.
        rig.publisher.reset();

        // Takes the freed memory back where the allocator allows and scribbles over it, so a timeout
        // left pending in the kernel fires on garbage within these periods instead of on leftovers.
        auto reused = std::make_unique<std::array<uint8_t, sizeof(SensorEventsPublisher)>>();
        reused->fill(0xA5);

        rig.frame->AddOrUpdateReading(MakeReading(*rig.a, 2.0F));
        k_sleep(K_MSEC(5 * CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS));
        zassert_equal(a_recorder->Count(), 1);
    }

    // And publishing goes on with the next publisher.
    Rig rig("after_destroyed");

    const auto b_recorder = std::make_shared<Recorder>();
    const auto sentinel_recorder = std::make_shared<Recorder>();
    AnySubscription b_binding = Bind(*rig.b, b_recorder);
    AnySubscription sentinel_binding = Bind(*rig.sentinel, sentinel_recorder);

    zassert_true(rig.Round({ { rig.b.get(), 5.0F } }, *sentinel_recorder));
    zassert_equal(b_recorder->Count(), 1);
    zassert_equal(b_recorder->At(0), 5.0F);
}
