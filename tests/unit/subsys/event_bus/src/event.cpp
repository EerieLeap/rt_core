#include <cstdint>
#include <string>
#include <variant>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event.h"

#include "test_events.h"

using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::EventHandler;
using eerie_leap::subsys::event_bus::EventPayload;

using event_bus_tests::TestEventType;
using event_bus_tests::TestPayloadType;

namespace {

using TestEvent = Event<TestEventType, TestPayloadType>;

} // namespace

ZTEST_SUITE(event_bus_event, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_event, test_an_event_keeps_its_type_and_source) {
    TestEvent event {
        .type = TestEventType::Beta,
        .payload = {},
        .source_id = "sensor_1"
    };

    zassert_equal(event.type, TestEventType::Beta);
    zassert_true(event.payload.empty());
    zassert_equal(event.source_id, std::string("sensor_1"));
}

ZTEST(event_bus_event, test_a_payload_holds_one_entry_per_payload_type) {
    TestEvent event {
        .type = TestEventType::Alpha,
        .payload = {},
        .source_id = "sensor_1"
    };

    event.payload[TestPayloadType::Value] = 42;
    event.payload[TestPayloadType::Label] = std::string("degrees");
    event.payload[TestPayloadType::Flag] = true;

    zassert_equal(event.payload.size(), 3U);
    zassert_equal(std::get<int>(event.payload.at(TestPayloadType::Value)), 42);
    zassert_equal(std::get<std::string>(event.payload.at(TestPayloadType::Label)), std::string("degrees"));
    zassert_true(std::get<bool>(event.payload.at(TestPayloadType::Flag)));
}

ZTEST(event_bus_event, test_reassigning_a_payload_entry_replaces_the_stored_alternative) {
    EventPayload<TestPayloadType> payload;

    payload[TestPayloadType::Value] = 42;
    payload[TestPayloadType::Value] = 1.5F;

    zassert_equal(payload.size(), 1U);
    zassert_true(std::holds_alternative<float>(payload.at(TestPayloadType::Value)));
    zassert_equal(std::get<float>(payload.at(TestPayloadType::Value)), 1.5F);
}

ZTEST(event_bus_event, test_the_payload_variant_keeps_int_and_uint32_apart) {
    EventPayload<TestPayloadType> payload;

    payload[TestPayloadType::Value] = static_cast<uint32_t>(7);

    zassert_true(std::holds_alternative<uint32_t>(payload.at(TestPayloadType::Value)));
    zassert_false(std::holds_alternative<int>(payload.at(TestPayloadType::Value)));
    zassert_equal(std::get<uint32_t>(payload.at(TestPayloadType::Value)), 7U);
}

ZTEST(event_bus_event, test_an_event_handler_receives_the_event_by_reference) {
    const TestEvent* observed = nullptr;

    EventHandler<TestEventType, TestPayloadType> handler = [&observed](const TestEvent& event) {
        observed = &event;
    };

    TestEvent event {
        .type = TestEventType::Gamma,
        .payload = {{TestPayloadType::Value, 11}},
        .source_id = "sensor_2"
    };

    handler(event);

    zassert_equal(observed, &event);
    zassert_equal(std::get<int>(observed->payload.at(TestPayloadType::Value)), 11);
}
