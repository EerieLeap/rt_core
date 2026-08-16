#include <algorithm>
#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "gpio.h"

LOG_MODULE_REGISTER(gpio_logger);

namespace eerie_leap::subsys::gpio {

using eerie_leap::subsys::threading::ScopedMutex;

int Gpio::Initialize() {
    LOG_INF("Gpio initialization started.");

    channel_callbacks_.clear();
    channel_callbacks_.reserve(gpio_specs_.size());

    for(size_t i = 0; i < gpio_specs_.size(); ++i) {
        if(!gpio_is_ready_dt(&gpio_specs_[i])) {
            LOG_ERR("Gpio device %s is not ready", gpio_specs_[i].port->name);

            return -1;
        }

        int res = gpio_pin_configure_dt(&gpio_specs_[i], GPIO_INPUT);
        if(res != 0) {
            LOG_ERR("Failed to configure Gpio channel %d, port %s, pin %d (%d)",
                static_cast<int>(i), gpio_specs_[i].port->name, gpio_specs_[i].pin, res);

            return res;
        }

        auto channel_callback = std::make_unique<ChannelCallback>();
        channel_callback->owner = this;
        channel_callback->channel = static_cast<int>(i);
        k_work_init(&channel_callback->work, ChannelChangedWorkHandler);

        channel_callbacks_.push_back(std::move(channel_callback));

        LOG_INF("Gpio channel %d configured: port %s, pin %d",
            static_cast<int>(i), gpio_specs_[i].port->name, gpio_specs_[i].pin);
    }

    LOG_INF("Gpio initialized successfully.");

    return 0;
}

bool Gpio::ReadChannel(int channel) {
    if(!IsChannelValid(channel))
        return false;

    return gpio_pin_get_dt(&gpio_specs_[channel]) > 0;
}

int Gpio::GetChannelCount() {
    return static_cast<int>(gpio_specs_.size());
}

int Gpio::RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) {
    if(!IsChannelValid(channel) || handler == nullptr)
        return ERR_INVALID_ARGUMENT;

    if(channel_callbacks_.size() != gpio_specs_.size())
        return ERR_NOT_INITIALIZED;

    auto& channel_callback = *channel_callbacks_[channel];
    int handler_id = 0;

    {
        ScopedMutex guard(lock_);

        if(!channel_callback.is_interrupt_attached) {
            gpio_init_callback(&channel_callback.callback, ChannelChangedCallback, BIT(gpio_specs_[channel].pin));

            int res = gpio_add_callback_dt(&gpio_specs_[channel], &channel_callback.callback);
            if(res != 0) {
                LOG_ERR("Failed to add Gpio callback for channel %d (%d)", channel, res);

                return ERR_INTERRUPT_REJECTED;
            }

            channel_callback.is_interrupt_attached = true;
        }

        handler_id = next_handler_id_++;
        channel_callback.handlers.push_back({
            .id = handler_id,
            .edge = edge,
            .handler = std::move(handler)});
    }

    int res = ApplyInterruptFlags(channel);
    if(res != 0) {
        RemoveChannelChangedHandler(channel, handler_id);

        return ERR_INTERRUPT_REJECTED;
    }

    return handler_id;
}

bool Gpio::RemoveChannelChangedHandler(int channel, int handler_id) {
    if(!IsChannelValid(channel) || handler_id <= 0)
        return false;

    if(channel_callbacks_.size() != gpio_specs_.size())
        return false;

    auto& channel_callback = *channel_callbacks_[channel];

    {
        ScopedMutex guard(lock_);

        auto handler = std::ranges::find_if(
            channel_callback.handlers,
            [handler_id](const ChannelHandler& channel_handler) { return channel_handler.id == handler_id; });

        if(handler == channel_callback.handlers.end())
            return false;

        channel_callback.handlers.erase(handler);
    }

    ApplyInterruptFlags(channel);

    return true;
}

bool Gpio::IsChannelValid(int channel) const {
    return channel >= 0 && static_cast<size_t>(channel) < gpio_specs_.size();
}

int Gpio::ApplyInterruptFlags(int channel) {
    gpio_flags_t flags = GPIO_INT_DISABLE;

    {
        ScopedMutex guard(lock_);

        flags = GetInterruptFlags(*channel_callbacks_[channel]);
    }

    int res = gpio_pin_interrupt_configure_dt(&gpio_specs_[channel], flags);
    if(res != 0)
        LOG_ERR("Failed to configure interrupt on Gpio channel %d (%d)", channel, res);

    return res;
}

gpio_flags_t Gpio::GetInterruptFlags(const ChannelCallback& channel_callback) {
    bool has_active = false;
    bool has_inactive = false;

    for(const auto& channel_handler : channel_callback.handlers) {
        has_active = has_active || channel_handler.edge != GpioEdge::INACTIVE;
        has_inactive = has_inactive || channel_handler.edge != GpioEdge::ACTIVE;
    }

    if(has_active && has_inactive)
        return GPIO_INT_EDGE_BOTH;

    if(has_active)
        return GPIO_INT_EDGE_TO_ACTIVE;

    if(has_inactive)
        return GPIO_INT_EDGE_TO_INACTIVE;

    return GPIO_INT_DISABLE;
}

void Gpio::ChannelChangedCallback(const device* dev, gpio_callback* callback, uint32_t pins) {
    ARG_UNUSED(dev);
    ARG_UNUSED(pins);

    auto* channel_callback = CONTAINER_OF(callback, ChannelCallback, callback);
    auto* owner = channel_callback->owner;

    int64_t now = k_uptime_get();
    if(now - channel_callback->last_event_time < DEBOUNCE_MS)
        return;

    channel_callback->last_event_time = now;

    // Latched here because the level can settle back before the bottom half runs.
    atomic_set(
        &channel_callback->pending_state,
        gpio_pin_get_dt(&owner->gpio_specs_[channel_callback->channel]) > 0 ? 1 : 0);

    k_work_submit(&channel_callback->work);
}

void Gpio::ChannelChangedWorkHandler(k_work* work) {
    auto* channel_callback = CONTAINER_OF(work, ChannelCallback, work);

    channel_callback->owner->DispatchChannelChanged(*channel_callback);
}

void Gpio::DispatchChannelChanged(ChannelCallback& channel_callback) {
    bool state = atomic_get(&channel_callback.pending_state) != 0;

    ScopedMutex guard(lock_);

    for(const auto& channel_handler : channel_callback.handlers) {
        if(channel_handler.edge == GpioEdge::ACTIVE && !state)
            continue;

        if(channel_handler.edge == GpioEdge::INACTIVE && state)
            continue;

        // Exceptions must not unwind into the work queue's C dispatch.
        try {
            channel_handler.handler(channel_callback.channel, state);
        } catch(const std::exception& e) {
            LOG_ERR("Gpio channel %d handler failed: %s", channel_callback.channel, e.what());
        } catch(...) {
            LOG_ERR("Gpio channel %d handler failed.", channel_callback.channel);
        }
    }
}

}  // namespace eerie_leap::subsys::gpio
