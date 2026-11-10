#include <atomic>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/gpio/composite_gpio.h"
#include "subsys/gpio/gpio_randomizer.h"

using namespace eerie_leap::subsys::gpio;

namespace {

// Reports a fixed channel count and a fixed Initialize() result; registrations hand out ids from 1,
// as every device does, so ids of different devices collide.
class StubGpio : public IGpio {
public:
    StubGpio(int channels, int initialize_result) :
        channels_(channels),
        initialize_result_(initialize_result) {}

    int Initialize() override {
        initialize_calls++;

        return initialize_result_;
    }
    bool ReadChannel(int channel) override {
        return channel == 1;
    }
    int GetChannelCount() override {
        return channels_;
    }
    int RegisterChannelChangedHandler(int channel, GpioEdge, GpioChannelHandler handler) override {
        registered_channel = channel;
        registered_handler = std::move(handler);

        return next_id_++;
    }
    bool RemoveChannelChangedHandler(int channel, int handler_id) override {
        removed_channel = channel;
        removed_id = handler_id;

        return true;
    }

    int initialize_calls = 0;
    int registered_channel = -1;
    GpioChannelHandler registered_handler;
    int removed_channel = -1;
    int removed_id = -1;

private:
    int channels_;
    int initialize_result_;
    int next_id_ = 1;
};

GpioRandomizerConfig Config(int channels, uint32_t seed) {
    return { .channel_count = channels, .hold_min_ms = 10, .hold_max_ms = 30, .seed = seed };
}

} // namespace

ZTEST_SUITE(composite_gpio, NULL, NULL, NULL, NULL, NULL);

ZTEST(composite_gpio, test_channels_are_concatenated_in_device_order) {
    auto first = std::make_shared<StubGpio>(3, 0);
    auto second = std::make_shared<StubGpio>(2, 0);
    CompositeGpio gpio({ first, second });

    zassert_equal(gpio.GetChannelCount(), 5);
    // StubGpio reads true on its local channel 1: global 1 (first) and 4 (second).
    zassert_false(gpio.ReadChannel(0));
    zassert_true(gpio.ReadChannel(1));
    zassert_false(gpio.ReadChannel(3));
    zassert_true(gpio.ReadChannel(4));
    zassert_false(gpio.ReadChannel(5));
    zassert_false(gpio.ReadChannel(-1));
}

ZTEST(composite_gpio, test_initialize_stops_at_the_first_failure) {
    auto first = std::make_shared<StubGpio>(1, -7);
    auto second = std::make_shared<StubGpio>(1, 0);
    CompositeGpio gpio({ first, second });

    zassert_equal(gpio.Initialize(), -7);
    zassert_equal(first->initialize_calls, 1);
    zassert_equal(second->initialize_calls, 0);
}

ZTEST(composite_gpio, test_initialize_initializes_every_device) {
    auto first = std::make_shared<StubGpio>(1, 0);
    auto second = std::make_shared<StubGpio>(1, 0);
    CompositeGpio gpio({ first, second });

    zassert_equal(gpio.Initialize(), 0);
    zassert_equal(first->initialize_calls, 1);
    zassert_equal(second->initialize_calls, 1);
}

ZTEST(composite_gpio, test_registration_goes_to_the_owning_device_with_a_local_channel) {
    auto first = std::make_shared<StubGpio>(3, 0);
    auto second = std::make_shared<StubGpio>(2, 0);
    CompositeGpio gpio({ first, second });

    int reported_channel = -1;
    bool reported_state = false;
    const int id = gpio.RegisterChannelChangedHandler(4, GpioEdge::BOTH, [&](int channel, bool state) {
        reported_channel = channel;
        reported_state = state;
    });

    zassert_true(id > 0);
    zassert_equal(first->registered_channel, -1);
    zassert_equal(second->registered_channel, 1);

    // The device calls back with its local channel; the handler hears the global one.
    second->registered_handler(1, true);
    zassert_equal(reported_channel, 4);
    zassert_true(reported_state);
}

ZTEST(composite_gpio, test_ids_are_unique_across_devices_and_removal_maps_back) {
    auto first = std::make_shared<StubGpio>(3, 0);
    auto second = std::make_shared<StubGpio>(2, 0);
    CompositeGpio gpio({ first, second });

    // Both devices hand out their id 1.
    const int id_a = gpio.RegisterChannelChangedHandler(0, GpioEdge::BOTH, [](int, bool) {});
    const int id_b = gpio.RegisterChannelChangedHandler(3, GpioEdge::BOTH, [](int, bool) {});

    zassert_true(id_a > 0 && id_b > 0 && id_a != id_b);

    zassert_false(gpio.RemoveChannelChangedHandler(0, id_b), "wrong channel");
    zassert_false(gpio.RemoveChannelChangedHandler(3, 0));
    zassert_false(gpio.RemoveChannelChangedHandler(9, id_b));

    zassert_true(gpio.RemoveChannelChangedHandler(3, id_b));
    zassert_equal(second->removed_channel, 0);
    zassert_equal(second->removed_id, 1);
    zassert_equal(first->removed_channel, -1);

    zassert_false(gpio.RemoveChannelChangedHandler(3, id_b), "already removed");

    zassert_true(gpio.RemoveChannelChangedHandler(0, id_a));
    zassert_equal(first->removed_channel, 0);
}

ZTEST(composite_gpio, test_invalid_registrations_are_rejected) {
    auto first = std::make_shared<StubGpio>(2, 0);
    CompositeGpio gpio({ first });

    zassert_equal(gpio.RegisterChannelChangedHandler(2, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_INVALID_ARGUMENT);
    zassert_equal(
        gpio.RegisterChannelChangedHandler(-1, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_INVALID_ARGUMENT
    );
    zassert_equal(gpio.RegisterChannelChangedHandler(0, GpioEdge::BOTH, nullptr), IGpio::ERR_INVALID_ARGUMENT);
}

ZTEST(composite_gpio, test_a_device_error_is_passed_through_and_not_recorded) {
    auto randomizer = std::make_shared<GpioRandomizer>(Config(2, 1));
    CompositeGpio gpio({ randomizer });

    // The randomizer is not initialized yet.
    zassert_equal(gpio.RegisterChannelChangedHandler(0, GpioEdge::BOTH, [](int, bool) {}), IGpio::ERR_NOT_INITIALIZED);
    zassert_false(gpio.RemoveChannelChangedHandler(0, 1));
}

ZTEST(composite_gpio, test_edges_of_two_randomizers_reach_handlers_with_global_channels) {
    auto first = std::make_shared<GpioRandomizer>(Config(2, 11));
    auto second = std::make_shared<GpioRandomizer>(Config(3, 12));
    CompositeGpio gpio({ first, second });
    zassert_equal(gpio.Initialize(), 0);

    std::atomic<int> low_calls = 0;
    std::atomic<int> high_calls = 0;
    std::atomic<int> wrong = 0;

    const int low_id = gpio.RegisterChannelChangedHandler(1, GpioEdge::BOTH, [&](int channel, bool) {
        wrong += channel == 1 ? 0 : 1;
        low_calls++;
    });
    const int high_id = gpio.RegisterChannelChangedHandler(4, GpioEdge::BOTH, [&](int channel, bool) {
        wrong += channel == 4 ? 0 : 1;
        high_calls++;
    });
    zassert_true(low_id > 0 && high_id > 0 && low_id != high_id);

    k_sleep(K_MSEC(800));

    zassert_true(gpio.RemoveChannelChangedHandler(1, low_id));
    zassert_true(gpio.RemoveChannelChangedHandler(4, high_id));

    zassert_true(low_calls > 5 && high_calls > 5);
    zassert_equal(wrong.load(), 0);

    const int low_after = low_calls;
    const int high_after = high_calls;
    k_sleep(K_MSEC(300));
    zassert_equal(low_calls.load(), low_after);
    zassert_equal(high_calls.load(), high_after);
}
