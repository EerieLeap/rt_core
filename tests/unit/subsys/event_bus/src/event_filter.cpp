#include <string>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_filter.h"

#include "test_events.h"

using eerie_leap::subsys::event_bus::AcceptAllFilter;
using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::EventFilter;

using event_bus_tests::TestEventType;
using event_bus_tests::TestPayloadType;

namespace {

using TestEvent = Event<TestEventType, TestPayloadType>;

constexpr uint32_t k_source_one = 0xA001;
constexpr uint32_t k_source_two = 0xA002;

struct SourceFilter {
    uint32_t source_id;

    bool operator()(const TestEvent& event) const { return event.source_id == source_id; }
};

struct MissingCallOperator { };

struct NonBooleanFilter {
    std::string operator()(const TestEvent&) const { return "no"; }
};

static_assert(EventFilter<AcceptAllFilter<TestEventType, TestPayloadType>, TestEventType, TestPayloadType>);
static_assert(EventFilter<SourceFilter, TestEventType, TestPayloadType>);
static_assert(!EventFilter<MissingCallOperator, TestEventType, TestPayloadType>);
static_assert(!EventFilter<NonBooleanFilter, TestEventType, TestPayloadType>);

TestEvent MakeEvent(TestEventType type, uint32_t source_id) {
    return TestEvent {
        .source_id = source_id,
        .type = type,
        .payload = {}
    };
}

} // namespace

ZTEST_SUITE(event_bus_event_filter, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_event_filter, test_AcceptAllFilter_accepts_every_event) {
    AcceptAllFilter<TestEventType, TestPayloadType> filter;

    zassert_true(filter(MakeEvent(TestEventType::Alpha, k_source_one)));
    zassert_true(filter(MakeEvent(TestEventType::Beta, k_source_two)));
    zassert_true(filter(MakeEvent(TestEventType::Gamma, 0)));
}

ZTEST(event_bus_event_filter, test_a_custom_filter_selects_events_by_source) {
    SourceFilter filter {k_source_one};

    zassert_true(filter(MakeEvent(TestEventType::Alpha, k_source_one)));
    zassert_false(filter(MakeEvent(TestEventType::Alpha, k_source_two)));
}
