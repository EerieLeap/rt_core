#pragma once

#include <atomic>
#include <stdexcept>

#include "subsys/gpio/i_gpio.h"

// An input controller with a few channels whose levels never change. Once armed with
// FailRegistrations(), a handler registration throws something the ISR reader factory does not
// catch, so a processing service that starts or restarts on a PHYSICAL_INDICATOR sensor fails and
// stays stopped. Registrations come from the configuration work queue and the arming from a test
// thread, hence the atomics.
namespace fake_gpio_test {

class FakeGpio : public eerie_leap::subsys::gpio::IGpio {
public:
    explicit FakeGpio(int channel_count = 4) :
        channel_count_(channel_count) {}

    int Initialize() override {
        return 0;
    }
    bool ReadChannel(int) override {
        return false;
    }
    int GetChannelCount() override {
        return channel_count_;
    }

    int RegisterChannelChangedHandler(
        int,
        eerie_leap::subsys::gpio::GpioEdge,
        eerie_leap::subsys::gpio::GpioChannelHandler
    ) override {
        if(is_failing_)
            throw std::logic_error("GPIO channel refused");

        handlers_++;

        return next_handler_id_++;
    }

    bool RemoveChannelChangedHandler(int, int) override {
        handlers_--;

        return true;
    }

    // From now on every registration throws.
    void FailRegistrations() {
        is_failing_ = true;
    }
    // The handlers that are registered now.
    [[nodiscard]] int GetHandlerCount() const {
        return handlers_;
    }

private:
    const int channel_count_;
    std::atomic<bool> is_failing_ = false;
    std::atomic<int> handlers_ = 0;
    std::atomic<int> next_handler_id_ = 1;
};

} // namespace fake_gpio_test
