#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/event_bus/erased_payload_view.h"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"

// Shared by the registry, binding and dispatch guard suites, which all resolve through the one
// EventChannelRegistry of this binary: Sensors -> primary, Logging -> secondary. Navigation and
// Settings are never registered here; the null cases rely on that.
//
// Everything below has external linkage on purpose (a named namespace, inline functions), so the
// three translation units share one fixture. A copy per unit would find Sensors already held by
// another unit's channel, and its publishes would never reach a subscription made through the
// registry.
namespace event_binding_tests {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::ErasedEventFilter;
using eerie_leap::subsys::event_bus::ErasedEventHandler;
using eerie_leap::subsys::event_bus::ErasedPayloadView;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
using eerie_leap::subsys::event_bus::EventData;

enum class BindingEventType : uint32_t {
    Updated = 0,
    Other = 1
};

enum class BindingPayloadType : uint32_t {
    Id = 0,
    Value = 1,
    Flag = 2
};

inline constexpr int BINDING_BUS_STACK_SIZE = 8192;
inline constexpr uint32_t BINDING_TEST_SOURCE = 0xB1D0;

class BindingTestChannel : public EventChannel<BindingEventType, BindingPayloadType> {
public:
    explicit BindingTestChannel(std::string name) :
        EventChannel(std::move(name)) {}
};

// Records what SubscribeEventBinding() hands the channel, so a test can evaluate the filter itself,
// and can refuse the next subscription the way a failed allocation would.
class RecordingChannel : public BindingTestChannel {
public:
    bool refuse_next = false;
    bool last_had_filter = false;
    ErasedEventFilter last_filter;

    explicit RecordingChannel(std::string name) :
        BindingTestChannel(std::move(name)) {}

    AnySubscription
    SubscribeErased(uint32_t event_type, ErasedEventFilter filter, ErasedEventHandler handler) override {

        last_had_filter = static_cast<bool>(filter);
        last_filter = filter;

        if(std::exchange(refuse_next, false))
            return nullptr;

        return BindingTestChannel::SubscribeErased(event_type, std::move(filter), std::move(handler));
    }
};

class BindingTestEventBus : public EventBus {
public:
    BindingTestEventBus() :
        EventBus("binding_test_bus", BINDING_BUS_STACK_SIZE) {}
};

// Declaration order matters: the bus is destroyed first, so ~EventBus detaches channels that are
// still alive.
struct BindingTestChannels {
    RecordingChannel primary{ "binding_primary" };
    BindingTestChannel secondary{ "binding_secondary" };
    BindingTestEventBus bus;

    // The first failed registration; a second fixture would see -EEXIST here.
    int registration = 0;

    BindingTestChannels() {
        bus.RegisterChannel(primary);
        bus.RegisterChannel(secondary);

        auto& registry = EventChannelRegistry::GetInstance();

        registration = registry.Register(EventChannelId::Sensors, primary);
        if(registration == 0)
            registration = registry.Register(EventChannelId::Logging, secondary);
    }
};

inline BindingTestChannels& RegisterBindingTestChannels() {
    static BindingTestChannels channels;

    return channels;
}

// Suite setup: fails the suite when the fixture is not the one the registry resolves to.
inline void* SetUpBindingTestChannels() {
    auto& channels = RegisterBindingTestChannels();

    zassert_ok(channels.registration, "The binding test channels could not be registered.");
    zassert_equal(EventChannelRegistry::GetInstance().Find(EventChannelId::Sensors), &channels.primary);

    return &channels;
}

// Before each case, so a refusal a failed case left armed cannot refuse a later subscription.
inline void ResetBindingTestChannels(void*) {
    RegisterBindingTestChannels().primary.refuse_next = false;
}

// A payload a test builds by hand, to evaluate a recorded filter directly.
class FakeView : public ErasedPayloadView {
private:
    std::span<const std::pair<uint32_t, EventData>> entries_;

public:
    explicit FakeView(std::span<const std::pair<uint32_t, EventData>> entries) :
        entries_(entries) {}

    const EventData* Find(uint32_t key) const override {
        for(const auto& [entry_key, value] : entries_) {
            if(entry_key == key)
                return &value;
        }

        return nullptr;
    }
};

// Dispatches on the calling thread, so a test needs no waits.
inline void PublishSync(BindingTestChannel& channel, BindingEventType type, BindingTestChannel::PayloadMap payload) {

    channel.Publish({ .source_id = BINDING_TEST_SOURCE, .type = type, .payload = std::move(payload) });
}

} // namespace event_binding_tests
