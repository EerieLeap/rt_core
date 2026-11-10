#include <algorithm>
#include <exception>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "subsys/threading/scoped_mutex.h"

#include "gpio_randomizer.h"

LOG_MODULE_REGISTER(gpio_randomizer_logger);

namespace eerie_leap::subsys::gpio {

using eerie_leap::subsys::threading::ScopedMutex;

GpioRandomizer::GpioRandomizer(const GpioRandomizerConfig& config) {
    k_mutex_init(&lock_);

    const uint32_t base_seed = config.seed != 0 ? config.seed : sys_rand32_get();
    const int channel_count = std::max(config.channel_count, 0);

    channels_.reserve(channel_count);
    for(int i = 0; i < channel_count; i++) {
        // Distinct streams per channel, still determined by the one seed.
        const uint32_t seed = base_seed + (static_cast<uint32_t>(i) + 1U) * 0x9E3779B9U;

        auto channel = std::make_unique<Channel>(
            this, i,
            GpioRandomSignalConfig{ .hold_min_ms = config.hold_min_ms, .hold_max_ms = config.hold_max_ms, .seed = seed }
        );
        k_work_init_delayable(&channel->work, FlipWorkHandler);

        channels_.push_back(std::move(channel));
    }
}

GpioRandomizer::~GpioRandomizer() {
    // The work item points into this object; wait out a run in progress.
    for(auto& channel : channels_) {
        k_work_sync sync{};
        k_work_cancel_delayable_sync(&channel->work, &sync);
    }
}

int GpioRandomizer::Initialize() {
    LOG_INF("Gpio Randomizer initialized: %d channels.", GetChannelCount());

    initialized_ = true;

    return 0;
}

bool GpioRandomizer::ReadChannel(int channel) {
    if(!IsChannelValid(channel))
        return false;

    ScopedMutex guard(lock_);

    return channels_[channel]->signal.At(k_uptime_get());
}

int GpioRandomizer::GetChannelCount() {
    return static_cast<int>(channels_.size());
}

int GpioRandomizer::RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) {
    if(!IsChannelValid(channel) || handler == nullptr)
        return ERR_INVALID_ARGUMENT;

    if(!initialized_)
        return ERR_NOT_INITIALIZED;

    auto& target = *channels_[channel];

    ScopedMutex guard(lock_);

    const int handler_id = next_handler_id_++;
    const bool first = target.handlers.empty();
    target.handlers.push_back({ .id = handler_id, .edge = edge, .handler = std::move(handler) });

    if(first) {
        // Edges are reported against the level at registration, not against an older dispatch.
        const int64_t now_ms = k_uptime_get();

        target.dispatched_level = target.signal.At(now_ms);
        ScheduleNextFlip(target, now_ms);
    }

    return handler_id;
}

bool GpioRandomizer::RemoveChannelChangedHandler(int channel, int handler_id) {
    if(!IsChannelValid(channel) || handler_id <= 0)
        return false;

    auto& target = *channels_[channel];

    ScopedMutex guard(lock_);

    const auto handler = std::ranges::find_if(target.handlers, [handler_id](const ChannelHandler& channel_handler) {
        return channel_handler.id == handler_id;
    });

    if(handler == target.handlers.end())
        return false;

    target.handlers.erase(handler);

    // A run that is already queued finds no handlers and does not reschedule.
    if(target.handlers.empty())
        k_work_cancel_delayable(&target.work);

    return true;
}

bool GpioRandomizer::IsChannelValid(int channel) const {
    return channel >= 0 && static_cast<size_t>(channel) < channels_.size();
}

void GpioRandomizer::ScheduleNextFlip(Channel& channel, int64_t now_ms) {
    const int64_t delay_ms = std::max<int64_t>(channel.signal.GetNextFlipMs() - now_ms, 1);

    k_work_reschedule(&channel.work, K_MSEC(delay_ms));
}

void GpioRandomizer::FlipWorkHandler(k_work* work) {
    auto* delayable = k_work_delayable_from_work(work);
    auto* channel = CONTAINER_OF(delayable, Channel, work);

    channel->owner->OnFlipDue(*channel);
}

void GpioRandomizer::OnFlipDue(Channel& channel) {
    ScopedMutex guard(lock_);

    if(channel.handlers.empty())
        return;

    // A late run (a busy work queue) sees several flips at once and reports the newest level.
    const int64_t now_ms = k_uptime_get();
    const bool state = channel.signal.At(now_ms);

    if(state != channel.dispatched_level) {
        channel.dispatched_level = state;

        for(const auto& channel_handler : channel.handlers) {
            if(channel_handler.edge == GpioEdge::ACTIVE && !state)
                continue;

            if(channel_handler.edge == GpioEdge::INACTIVE && state)
                continue;

            // Exceptions must not unwind into the work queue's C dispatch.
            try {
                channel_handler.handler(channel.index, state);
            } catch(const std::exception& e) {
                LOG_ERR("Gpio randomizer channel %d handler failed: %s", channel.index, e.what());
            } catch(...) {
                LOG_ERR("Gpio randomizer channel %d handler failed.", channel.index);
            }
        }
    }

    // A handler may have removed itself and the last one.
    if(!channel.handlers.empty())
        ScheduleNextFlip(channel, now_ms);
}

} // namespace eerie_leap::subsys::gpio
