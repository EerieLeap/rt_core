#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include "i_gpio.h"

// The header is pulled in by domains that can be built without the subsystem.
#ifndef CONFIG_EERIE_LEAP_GPIO_DEBOUNCE_MS
#define CONFIG_EERIE_LEAP_GPIO_DEBOUNCE_MS 50
#endif

namespace eerie_leap::subsys::gpio {

class Gpio : public IGpio {
protected:
    static constexpr int64_t DEBOUNCE_MS = CONFIG_EERIE_LEAP_GPIO_DEBOUNCE_MS;

    struct ChannelHandler {
        int id;
        GpioEdge edge;
        GpioChannelHandler handler;
    };

    // The interrupt only latches the level and submits the bottom half, so
    // handlers run in thread context and may block.
    // NOTE: callback and work must keep fixed offsets, both trampolines recover
    // the owning channel through CONTAINER_OF.
    struct ChannelCallback {
        gpio_callback callback{};
        k_work work{};
        Gpio* owner = nullptr;
        int channel = -1;
        int64_t last_event_time = 0;
        atomic_t pending_state = ATOMIC_INIT(0);
        bool is_interrupt_attached = false;
        std::vector<ChannelHandler> handlers;
    };

    std::vector<gpio_dt_spec> gpio_specs_;
    std::vector<std::unique_ptr<ChannelCallback>> channel_callbacks_;

    mutable k_mutex lock_;
    int next_handler_id_ = 1;

    bool IsChannelValid(int channel) const;
    int ApplyInterruptFlags(int channel);
    static gpio_flags_t GetInterruptFlags(const ChannelCallback& channel_callback);
    static void ChannelChangedCallback(const device* dev, gpio_callback* callback, uint32_t pins);
    static void ChannelChangedWorkHandler(k_work* work);
    void DispatchChannelChanged(ChannelCallback& channel_callback);

public:
    explicit Gpio(std::vector<gpio_dt_spec> gpio_specs) : gpio_specs_(std::move(gpio_specs)) { k_mutex_init(&lock_); }

    int Initialize() override;
    bool ReadChannel(int channel) override;
    int GetChannelCount() override;

    int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) override;
    bool RemoveChannelChangedHandler(int channel, int handler_id) override;
};

}  // namespace eerie_leap::subsys::gpio
