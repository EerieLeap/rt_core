#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/configuration/parsers/logging_control_configuration_validator.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"
#include "subsys/event_bus/event_channel_id.h"
#include "utilities/memory/memory_resource_manager.h"

using namespace eerie_leap::utilities::memory;
using eerie_leap::domain::logging_domain::control::configuration::parsers::LoggingControlConfigurationValidator;
using eerie_leap::domain::logging_domain::control::models::LoggingControlBinding;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::LAST_EVENT_CHANNEL_ID;

ZTEST_SUITE(logging_control_configuration_validator, NULL, NULL, NULL, NULL, NULL);

namespace {

LoggingControlBinding MakeTrigger(EventChannelId channel = EventChannelId::Sensors) {
    LoggingControlBinding trigger;
    trigger.event.channel = channel;
    trigger.event.event_type = 0;
    trigger.event.payload_key = 1;

    return trigger;
}

LoggingControlConfiguration MakeConfiguration(std::vector<LoggingControlBinding> triggers) {
    LoggingControlConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    for(auto& trigger : triggers)
        configuration.triggers.push_back(std::move(trigger));

    return configuration;
}

// The message of the exception Validate() throws, empty when it does not.
std::string Rejection(const LoggingControlConfiguration& configuration) {
    try {
        LoggingControlConfigurationValidator::Validate(configuration);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return {};
}

} // namespace

ZTEST(logging_control_configuration_validator, test_NoTriggersIsValid) {
    zassert_true(Rejection(MakeConfiguration({})).empty());
}

ZTEST(logging_control_configuration_validator, test_AnyTargetUidIsValid) {
    auto all = MakeTrigger();
    all.target_uid = 0;
    auto last = MakeTrigger();
    last.target_uid = 0xFFFFFFFF;

    zassert_true(Rejection(MakeConfiguration({ all, last })).empty());
}

ZTEST(logging_control_configuration_validator, test_AChannelMustBeOneThatExists) {
    const auto none = Rejection(MakeConfiguration({ MakeTrigger(), MakeTrigger(EventChannelId::None) }));
    zassert_true(none.find("Invalid logging control configuration.") == 0, "%s", none.c_str());
    zassert_true(none.contains("Trigger 1 names an unknown event channel."), "%s", none.c_str());

    const auto past_last = Rejection(
        MakeConfiguration({ MakeTrigger(static_cast<EventChannelId>(std::to_underlying(LAST_EVENT_CHANNEL_ID) + 1)) })
    );
    zassert_true(past_last.contains("Trigger 0 names an unknown event channel."), "%s", past_last.c_str());
}

ZTEST(logging_control_configuration_validator, test_AChannelThatThisUnitDoesNotRegisterIsValid) {
    // Whether a unit registers a channel is the registry's question, not the stored format's.
    zassert_true(Rejection(MakeConfiguration(
                               { MakeTrigger(EventChannelId::Navigation), MakeTrigger(EventChannelId::Settings),
                                   MakeTrigger(EventChannelId::Logging) }
                           ))
            .empty());
}

ZTEST(logging_control_configuration_validator, test_ASelectorMustBeTextAnIntegerOrABool) {
    auto text = MakeTrigger();
    text.event.selector_value = std::pmr::string("ignition");
    auto number = MakeTrigger();
    number.event.selector_value = 12;
    auto flag = MakeTrigger();
    flag.event.selector_value = false;
    zassert_true(
        Rejection(MakeConfiguration({ text, number, flag, MakeTrigger() })).empty(), "Three kinds of selector and none"
    );

    auto real = MakeTrigger();
    real.event.selector_value = 1.5;
    const auto real_message = Rejection(MakeConfiguration({ MakeTrigger(), real }));
    zassert_true(
        real_message.contains("Trigger 1 selector value must be text, an integer or a bool."), "%s",
        real_message.c_str()
    );

    auto numbers = MakeTrigger();
    numbers.event.selector_value = std::pmr::vector<int>{ 1, 2 };
    zassert_false(Rejection(MakeConfiguration({ numbers })).empty(), "A list cannot select");
}

ZTEST(logging_control_configuration_validator, test_TheEventTypeAndTheKeysAreChannelLocal) {
    auto trigger = MakeTrigger();
    trigger.event.event_type = 0xFFFFFFFF;
    trigger.event.payload_key = 0xFFFFFFFF;
    trigger.event.selector_key = 0xFFFFFFFF;
    trigger.event.selector_value = 1;

    zassert_true(Rejection(MakeConfiguration({ trigger })).empty(), "The validator does not know the channel's enums");
}

ZTEST(logging_control_configuration_validator, test_AtMostSixteenTriggers) {
    zassert_true(Rejection(MakeConfiguration(std::vector<LoggingControlBinding>(16, MakeTrigger()))).empty());

    const auto message = Rejection(MakeConfiguration(std::vector<LoggingControlBinding>(17, MakeTrigger())));
    zassert_true(message.contains("At most 16 triggers."), "%s", message.c_str());
}
