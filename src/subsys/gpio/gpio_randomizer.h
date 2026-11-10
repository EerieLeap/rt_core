#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "gpio_random_signal.h"
#include "i_gpio.h"

namespace eerie_leap::subsys::gpio {

struct GpioRandomizerConfig {
    int channel_count = 4;
    uint32_t hold_min_ms = 100;
    uint32_t hold_max_ms = 5000;
    uint32_t seed = 0; // 0: from the entropy source, so each boot differs.
};

// A bench and development GPIO: every channel is a level that flips after random hold times
// (GpioRandomSignal). Handlers get the same contract as Gpio: edge filters, newest level, system
// work queue. A flip is scheduled only while the channel has handlers.
class GpioRandomizer : public IGpio {
private:
    struct ChannelHandler {
        int id;
        GpioEdge edge;
        GpioChannelHandler handler;
    };

    // Heap-allocated and never moved: the delayable work is found again by CONTAINER_OF.
    struct Channel {
        Channel(GpioRandomizer* owner, int index, const GpioRandomSignalConfig& config) :
            owner(owner),
            index(index),
            signal(config) {}

        GpioRandomizer* owner;
        int index;
        GpioRandomSignal signal;
        k_work_delayable work{};
        bool dispatched_level = false;
        std::vector<ChannelHandler> handlers;
    };

    std::vector<std::unique_ptr<Channel>> channels_;
    bool initialized_ = false;

    mutable k_mutex lock_{};
    int next_handler_id_ = 1;

    [[nodiscard]] bool IsChannelValid(int channel) const;
    static void FlipWorkHandler(k_work* work);
    void OnFlipDue(Channel& channel);
    static void ScheduleNextFlip(Channel& channel, int64_t now_ms);

public:
    explicit GpioRandomizer(const GpioRandomizerConfig& config);
    ~GpioRandomizer() override;

    GpioRandomizer(const GpioRandomizer&) = delete;
    GpioRandomizer& operator=(const GpioRandomizer&) = delete;
    GpioRandomizer(GpioRandomizer&&) = delete;
    GpioRandomizer& operator=(GpioRandomizer&&) = delete;

    int Initialize() override;
    bool ReadChannel(int channel) override;
    int GetChannelCount() override;

    int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) override;
    bool RemoveChannelChangedHandler(int channel, int handler_id) override;
};

} // namespace eerie_leap::subsys::gpio
