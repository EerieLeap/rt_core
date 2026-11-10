#include <algorithm>
#include <utility>

#include "subsys/threading/scoped_mutex.h"

#include "composite_gpio.h"

namespace eerie_leap::subsys::gpio {

using eerie_leap::subsys::threading::ScopedMutex;

CompositeGpio::CompositeGpio(std::vector<std::shared_ptr<IGpio>> devices) :
    devices_(std::move(devices)) {
    k_mutex_init(&lock_);
}

int CompositeGpio::Initialize() {
    for(const auto& device : devices_) {
        const int res = device->Initialize();
        if(res != 0)
            return res;
    }

    return 0;
}

int CompositeGpio::OffsetOf(int device) const {
    int offset = 0;
    for(int i = 0; i < device; i++)
        offset += devices_[i]->GetChannelCount();

    return offset;
}

bool CompositeGpio::Resolve(int channel, Resolved& resolved) const {
    if(channel < 0)
        return false;

    int offset = 0;
    for(size_t i = 0; i < devices_.size(); i++) {
        const int count = devices_[i]->GetChannelCount();
        if(channel < offset + count) {
            resolved = { .device = static_cast<int>(i), .local_channel = channel - offset };

            return true;
        }

        offset += count;
    }

    return false;
}

bool CompositeGpio::ReadChannel(int channel) {
    Resolved resolved{};
    if(!Resolve(channel, resolved))
        return false;

    return devices_[resolved.device]->ReadChannel(resolved.local_channel);
}

int CompositeGpio::GetChannelCount() {
    return OffsetOf(static_cast<int>(devices_.size()));
}

int CompositeGpio::RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) {
    Resolved resolved{};
    if(!Resolve(channel, resolved) || handler == nullptr)
        return ERR_INVALID_ARGUMENT;

    const int offset = OffsetOf(resolved.device);

    const int inner_id = devices_[resolved.device]->RegisterChannelChangedHandler(
        resolved.local_channel, edge, [offset, handler = std::move(handler)](int local_channel, bool state) {
            handler(offset + local_channel, state);
        }
    );
    if(inner_id < 0)
        return inner_id;

    ScopedMutex guard(lock_);

    // The devices' ids can collide, so the composite hands out its own.
    const int id = next_handler_id_++;
    registrations_.push_back({ .id = id, .channel = channel, .device = resolved.device, .inner_id = inner_id });

    return id;
}

bool CompositeGpio::RemoveChannelChangedHandler(int channel, int handler_id) {
    Resolved resolved{};
    if(handler_id <= 0 || !Resolve(channel, resolved))
        return false;

    Registration registration{};

    {
        ScopedMutex guard(lock_);

        const auto found = std::ranges::find_if(registrations_, [&](const Registration& r) {
            return r.id == handler_id && r.channel == channel;
        });
        if(found == registrations_.end())
            return false;

        registration = *found;
        registrations_.erase(found);
    }

    return devices_[registration.device]->RemoveChannelChangedHandler(resolved.local_channel, registration.inner_id);
}

} // namespace eerie_leap::subsys::gpio
