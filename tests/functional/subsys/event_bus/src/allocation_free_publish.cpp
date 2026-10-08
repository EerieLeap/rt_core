// A numeric payload travels from PublishAsync() through the queue to the subscriber without a
// single heap allocation: the payload is inline, the queue is a ring sized once, and dispatch
// snapshots the subscribers into an inline buffer.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <variant>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/i_event_bus.h"

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::subsys::event_bus::EventPayload;
using eerie_leap::subsys::event_bus::IEventBus;
using eerie_leap::subsys::event_bus::k_max_payload_entries;

// Replaces the global allocation functions for this test binary so every allocation is counted.
namespace {

size_t g_allocations = 0;

void* CountedAllocate(size_t size) {
    g_allocations++;

    void* pointer = std::malloc(size == 0 ? 1 : size);
    if(pointer == nullptr)
        throw std::bad_alloc();

    return pointer;
}

} // namespace

void* operator new(size_t size) { return CountedAllocate(size); }
void* operator new[](size_t size) { return CountedAllocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { g_allocations++; return std::malloc(size == 0 ? 1 : size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { g_allocations++; return std::malloc(size == 0 ? 1 : size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

namespace {

enum class ProbeEventType : uint32_t { DataUpdated = 0 };
enum class ProbePayloadType : uint32_t { SensorId = 0, Value = 1 };

using ProbeEvent = Event<ProbeEventType, ProbePayloadType>;

class ProbeChannel : public EventChannel<ProbeEventType, ProbePayloadType> {
public:
    explicit ProbeChannel(size_t capacity) : EventChannel("probe", capacity) {}
};

class ManualBus : public IEventBus {
public:
    void Wake() override { }
};

} // namespace

ZTEST_SUITE(allocation_free_publish, NULL, NULL, NULL, NULL, NULL);

ZTEST(allocation_free_publish, test_publish_queue_and_dispatch_do_not_allocate) {
    ProbeChannel channel(8);
    ManualBus bus;
    channel.OnRegistered(&bus);

    uint32_t last_id = 0;
    float last_value = 0.0F;
    size_t delivered = 0;

    AnySubscription subscription = CreateScopedSubscription(channel, ProbeEventType::DataUpdated,
        [&](const ProbeEvent& event) {
            if(auto it = event.payload.find(ProbePayloadType::SensorId); it != event.payload.end())
                last_id = std::get<uint32_t>(it->second);
            if(auto it = event.payload.find(ProbePayloadType::Value); it != event.payload.end())
                last_value = std::get<float>(it->second);

            delivered++;
        });

    // Warm up: nothing should allocate even on the first pass, but the comparison is what counts.
    channel.PublishAsync({ .source_id = 1, .type = ProbeEventType::DataUpdated,
        .payload = { { ProbePayloadType::SensorId, uint32_t { 7 } }, { ProbePayloadType::Value, 1.0F } } });
    zassert_true(channel.DrainOne());

    const size_t before = g_allocations;

    for(uint32_t i = 0; i < 100; i++) {
        channel.PublishAsync({ .source_id = 1, .type = ProbeEventType::DataUpdated,
            .payload = { { ProbePayloadType::SensorId, uint32_t { 100 + i } }, { ProbePayloadType::Value, static_cast<float>(i) } } });
        zassert_true(channel.DrainOne());
    }

    zassert_equal(g_allocations - before, 0U, "%zu allocations across 100 publish/drain cycles", g_allocations - before);
    zassert_equal(delivered, 101U);
    zassert_equal(last_id, 199U);
    zassert_equal(last_value, 99.0F);
}

ZTEST(allocation_free_publish, test_a_full_ring_sheds_the_oldest_without_allocating) {
    ProbeChannel channel(4);
    ManualBus bus;
    channel.OnRegistered(&bus);

    std::vector<uint32_t> seen;
    seen.reserve(16);
    AnySubscription subscription = CreateScopedSubscription(channel, ProbeEventType::DataUpdated,
        [&](const ProbeEvent& event) { seen.push_back(std::get<uint32_t>(event.payload.at(ProbePayloadType::SensorId))); });

    const size_t before = g_allocations;

    for(uint32_t i = 0; i < 6; i++) {
        channel.PublishAsync({ .source_id = i, .type = ProbeEventType::DataUpdated,
            .payload = { { ProbePayloadType::SensorId, i } } });
    }

    while(channel.DrainOne()) { }

    zassert_equal(g_allocations - before, 0U);
    zassert_equal(seen.size(), 4U, "Four slots: two oldest were shed");
    zassert_equal(seen[0], 2U);
    zassert_equal(seen[3], 5U);
}

ZTEST(allocation_free_publish, test_inline_payload_keeps_map_semantics) {
    EventPayload<ProbePayloadType> payload;

    zassert_true(payload.empty());
    payload[ProbePayloadType::Value] = 1.5F;
    payload[ProbePayloadType::Value] = 2.5F;
    zassert_equal(payload.size(), 1U, "operator[] replaces");
    zassert_equal(std::get<float>(payload.at(ProbePayloadType::Value)), 2.5F);

    auto [it, inserted] = payload.emplace(ProbePayloadType::Value, 9);
    zassert_false(inserted, "emplace keeps an existing entry");
    zassert_true(std::holds_alternative<float>(it->second));

    zassert_true(payload.emplace(ProbePayloadType::SensorId, uint32_t { 3 }).second);
    zassert_equal(payload.size(), 2U);
    zassert_true(payload.find(ProbePayloadType::SensorId) != payload.end());
    zassert_equal(payload.erase(ProbePayloadType::Value), 1U);
    zassert_equal(payload.size(), 1U);
    zassert_true(payload.find(ProbePayloadType::Value) == payload.end());
    zassert_equal(std::get<uint32_t>(payload.begin()->second), 3U, "The remaining entry moved up");

    zassert_equal(k_max_payload_entries, 6U);
}
