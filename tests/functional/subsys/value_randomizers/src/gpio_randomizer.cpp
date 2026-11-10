#include <atomic>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/gpio/gpio_randomizer.h"

using namespace eerie_leap::subsys::gpio;

namespace {

GpioRandomizerConfig Config(int channels = 3) {
    return { .channel_count = channels, .hold_min_ms = 10, .hold_max_ms = 30, .seed = 5 };
}

std::unique_ptr<GpioRandomizer> MakeInitialized(int channels = 3) {
    auto gpio = std::make_unique<GpioRandomizer>(Config(channels));
    zassert_equal(gpio->Initialize(), 0);

    return gpio;
}

} // namespace

ZTEST_SUITE(gpio_randomizer, NULL, NULL, NULL, NULL, NULL);

ZTEST(gpio_randomizer, test_reports_the_channel_count) {
    GpioRandomizer gpio(Config(7));

    zassert_equal(gpio.GetChannelCount(), 7);
}

ZTEST(gpio_randomizer, test_read_invalid_channel_is_false_and_valid_channel_flips) {
    auto gpio = MakeInitialized();

    zassert_false(gpio->ReadChannel(-1));
    zassert_false(gpio->ReadChannel(3));

    int highs = 0;
    for(int i = 0; i < 100; i++) {
        highs += gpio->ReadChannel(1) ? 1 : 0;
        k_sleep(K_MSEC(7));
    }

    zassert_true(highs > 10 && highs < 90, "%d", highs);
}

ZTEST(gpio_randomizer, test_registration_before_initialize_fails) {
    GpioRandomizer gpio(Config());

    zassert_equal(gpio.RegisterChannelChangedHandler(0, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_NOT_INITIALIZED);
}

ZTEST(gpio_randomizer, test_invalid_registrations_are_rejected) {
    auto gpio = MakeInitialized();

    zassert_equal(
        gpio->RegisterChannelChangedHandler(-1, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_INVALID_ARGUMENT
    );
    zassert_equal(
        gpio->RegisterChannelChangedHandler(3, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_INVALID_ARGUMENT
    );
    zassert_equal(gpio->RegisterChannelChangedHandler(0, GpioEdge::BOTH, nullptr), IGpio::ERR_INVALID_ARGUMENT);
}

ZTEST(gpio_randomizer, test_a_both_handler_sees_alternating_levels_of_its_channel) {
    auto gpio = MakeInitialized();

    std::atomic<int> calls = 0;
    std::atomic<int> wrong_channel = 0;
    std::atomic<int> repeated = 0;
    std::atomic<int> last_state = -1;

    const int id = gpio->RegisterChannelChangedHandler(2, GpioEdge::BOTH, [&](int channel, bool state) {
        if(channel != 2)
            wrong_channel++;
        if(last_state.exchange(state ? 1 : 0) == (state ? 1 : 0))
            repeated++;
        calls++;
    });
    zassert_true(id > 0);

    k_sleep(K_MSEC(1000));
    zassert_true(gpio->RemoveChannelChangedHandler(2, id));

    zassert_true(calls > 20, "%d", calls.load());
    zassert_equal(wrong_channel.load(), 0);
    zassert_equal(repeated.load(), 0, "every call is an edge");
}

ZTEST(gpio_randomizer, test_edge_filters_select_the_direction) {
    auto gpio = MakeInitialized();

    std::atomic<int> active_calls = 0;
    std::atomic<int> active_low = 0;
    std::atomic<int> inactive_calls = 0;
    std::atomic<int> inactive_high = 0;

    const int a = gpio->RegisterChannelChangedHandler(0, GpioEdge::ACTIVE, [&](int, bool state) {
        active_calls++;
        active_low += state ? 0 : 1;
    });
    const int b = gpio->RegisterChannelChangedHandler(0, GpioEdge::INACTIVE, [&](int, bool state) {
        inactive_calls++;
        inactive_high += state ? 1 : 0;
    });
    zassert_true(a > 0 && b > 0 && a != b);

    k_sleep(K_MSEC(1000));

    zassert_true(active_calls > 5 && inactive_calls > 5);
    zassert_equal(active_low.load(), 0);
    zassert_equal(inactive_high.load(), 0);
    // Rising and falling edges alternate, so the counts differ by at most one.
    zassert_true(std::abs(active_calls.load() - inactive_calls.load()) <= 1);

    gpio->RemoveChannelChangedHandler(0, a);
    gpio->RemoveChannelChangedHandler(0, b);
}

ZTEST(gpio_randomizer, test_removal_stops_the_handler_and_keeps_the_others) {
    auto gpio = MakeInitialized();

    std::atomic<int> first = 0;
    std::atomic<int> second = 0;

    const int id1 = gpio->RegisterChannelChangedHandler(1, GpioEdge::BOTH, [&](int, bool) {
        first++;
    });
    const int id2 = gpio->RegisterChannelChangedHandler(1, GpioEdge::BOTH, [&](int, bool) {
        second++;
    });

    k_sleep(K_MSEC(400));
    zassert_true(first > 0);

    zassert_true(gpio->RemoveChannelChangedHandler(1, id1));
    zassert_false(gpio->RemoveChannelChangedHandler(1, id1), "already removed");
    zassert_false(gpio->RemoveChannelChangedHandler(0, id2), "another channel");
    zassert_false(gpio->RemoveChannelChangedHandler(1, 0));
    zassert_false(gpio->RemoveChannelChangedHandler(9, id2));

    const int first_after_removal = first;
    const int second_before = second;

    k_sleep(K_MSEC(400));

    zassert_equal(first.load(), first_after_removal);
    zassert_true(second > second_before);

    zassert_true(gpio->RemoveChannelChangedHandler(1, id2));

    const int second_after_removal = second;
    k_sleep(K_MSEC(300));

    zassert_equal(second.load(), second_after_removal);
}

ZTEST(gpio_randomizer, test_a_channel_without_handlers_raises_nothing_and_can_register_again) {
    auto gpio = MakeInitialized();

    std::atomic<int> calls = 0;
    int id = gpio->RegisterChannelChangedHandler(0, GpioEdge::BOTH, [&](int, bool) {
        calls++;
    });
    gpio->RemoveChannelChangedHandler(0, id);

    k_sleep(K_MSEC(300));
    zassert_equal(calls.load(), 0);

    id = gpio->RegisterChannelChangedHandler(0, GpioEdge::BOTH, [&](int, bool) {
        calls++;
    });
    zassert_true(id > 0);
    k_sleep(K_MSEC(500));

    zassert_true(calls > 3);
    gpio->RemoveChannelChangedHandler(0, id);
}

ZTEST(gpio_randomizer, test_a_throwing_handler_does_not_stop_the_dispatch) {
    auto gpio = MakeInitialized();

    std::atomic<int> thrown = 0;
    std::atomic<int> survivor = 0;

    const int ids[3] = {
        gpio->RegisterChannelChangedHandler(
            0, GpioEdge::BOTH,
            [&](int, bool) {
                thrown++;
                throw std::runtime_error("handler failure");
            }
        ),
        gpio->RegisterChannelChangedHandler(
            0, GpioEdge::BOTH,
            [&](int, bool) {
                survivor++;
            }
        ),
        gpio->RegisterChannelChangedHandler(
            0, GpioEdge::BOTH,
            [&](int, bool) {
                throw 42; // NOLINT(bugprone-std-exception-baseclass): exercises the catch-all
            }
        ),
    };

    k_sleep(K_MSEC(600));

    // Removal waits for a dispatch in progress, so the counters are settled.
    for(const int id : ids)
        zassert_true(gpio->RemoveChannelChangedHandler(0, id));

    zassert_true(thrown > 3);
    zassert_equal(thrown.load(), survivor.load(), "the handler after the throwing one still runs, every time");
}

ZTEST(gpio_randomizer, test_a_slow_handler_gets_the_newest_level_not_a_backlog) {
    auto gpio = MakeInitialized();

    std::atomic<int> calls = 0;
    std::atomic<int> repeated = 0;
    std::atomic<int> last_state = -1;

    const int id = gpio->RegisterChannelChangedHandler(0, GpioEdge::BOTH, [&](int, bool state) {
        if(last_state.exchange(state ? 1 : 0) == (state ? 1 : 0))
            repeated++;
        calls++;

        // Many flips (10-30 ms holds) pass while this blocks the work queue.
        k_sleep(K_MSEC(120));
    });

    k_sleep(K_MSEC(2000));
    gpio->RemoveChannelChangedHandler(0, id);

    // Without coalescing there would be about 100 flips to deliver; at most one run per 120 ms.
    zassert_true(calls > 3 && calls <= 2000 / 120 + 2, "%d", calls.load());
    zassert_equal(repeated.load(), 0);
}

ZTEST(gpio_randomizer, test_destruction_with_active_handlers_is_safe) {
    std::atomic<int> calls = 0;

    {
        auto gpio = MakeInitialized();

        gpio->RegisterChannelChangedHandler(0, GpioEdge::BOTH, [&](int, bool) {
            calls++;
        });
        gpio->RegisterChannelChangedHandler(2, GpioEdge::BOTH, [&](int, bool) {
            calls++;
        });

        k_sleep(K_MSEC(200));
    }

    const int at_destruction = calls;
    k_sleep(K_MSEC(300));

    zassert_equal(calls.load(), at_destruction, "no work survives the object");
}
