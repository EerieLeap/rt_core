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

struct SourceFilter {
    std::string source_id;

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

TestEvent MakeEvent(TestEventType type, std::string source_id) {
    return TestEvent {
        .type = type,
        .payload = {},
        .source_id = std::move(source_id)
    };
}

} // namespace

ZTEST_SUITE(event_bus_event_filter, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_event_filter, test_AcceptAllFilter_accepts_every_event) {
    AcceptAllFilter<TestEventType, TestPayloadType> filter;

    zassert_true(filter(MakeEvent(TestEventType::Alpha, "sensor_1")));
    zassert_true(filter(MakeEvent(TestEventType::Beta, "sensor_2")));
    zassert_true(filter(MakeEvent(TestEventType::Gamma, "")));
}

ZTEST(event_bus_event_filter, test_a_custom_filter_selects_events_by_source) {
    SourceFilter filter {"sensor_1"};

    zassert_true(filter(MakeEvent(TestEventType::Alpha, "sensor_1")));
    zassert_false(filter(MakeEvent(TestEventType::Alpha, "sensor_2")));
}
