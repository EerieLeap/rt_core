#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "domain/logging_domain/control/configuration/parsers/logging_control_configuration_cbor_parser.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"
#include "utilities/memory/memory_resource_manager.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::domain::logging_domain::control::configuration::parsers::LoggingControlConfigurationCborParser;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;
using eerie_leap::subsys::event_bus::EventChannelId;

ZTEST_SUITE(logging_control_configuration_parser, NULL, NULL, NULL, NULL, NULL);

namespace {

using Bytes = std::vector<uint8_t>;

// {Sensors (1), DataUpdated (0), Value (1), selector SensorId (0) = "ab", target 0x01020304}. zcbor writes lists with
// indefinite length, so each list is 9F ... FF.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes TEXT_SELECTOR = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0x62, 0x61, 0x62, 0x1A, 0x01, 0x02, 0x03,
    0x04, 0xFF, 0xFF, 0xFF };
// The same without a selector: false, key 0, the value written as int 0, and target 0 (every unit).
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes NO_SELECTOR = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes K_EMPTY = { 0x9F, 0x9F, 0xFF, 0xFF };

LoggingTrigger MakeTrigger(uint32_t event_type = 0, uint32_t payload_key = 1, uint32_t selector_key = 0) {
    LoggingTrigger trigger;
    trigger.binding.channel = EventChannelId::Sensors;
    trigger.binding.event_type = event_type;
    trigger.binding.payload_key = payload_key;
    trigger.binding.selector_key = selector_key;

    return trigger;
}

LoggingControlConfiguration MakeConfiguration(std::vector<LoggingTrigger> triggers) {
    LoggingControlConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    for(auto& trigger : triggers)
        configuration.triggers.push_back(std::move(trigger));

    return configuration;
}

// Encodes a configuration the way the manager stores it.
Bytes Encode(const LoggingControlConfiguration& configuration) {
    LoggingControlConfigurationCborParser parser;
    CborSerializer<CborLoggingControlConfig> serializer;

    const auto cbor_config = parser.Serialize(configuration);
    const auto bytes = serializer.Serialize(*cbor_config);

    return { bytes.begin(), bytes.end() };
}

// Decodes stored bytes to the CBOR structure; null for bytes that are not the schema's.
pmr_unique_ptr<CborLoggingControlConfig> Decode(const Bytes& bytes) {
    CborSerializer<CborLoggingControlConfig> serializer;

    return serializer.Deserialize(bytes);
}

void AssertSame(const LoggingControlConfiguration& expected, const LoggingControlConfiguration& actual) {
    zassert_equal(actual.triggers.size(), expected.triggers.size());

    for(size_t i = 0; i < expected.triggers.size(); i++) {
        const auto& a = expected.triggers[i].binding;
        const auto& b = actual.triggers[i].binding;

        zassert_equal(b.channel, a.channel, "trigger %u", static_cast<unsigned>(i));
        zassert_equal(b.event_type, a.event_type);
        zassert_equal(b.payload_key, a.payload_key);
        zassert_equal(b.selector_key, a.selector_key);
        zassert_equal(
            actual.triggers[i].target_uid, expected.triggers[i].target_uid, "trigger %u target",
            static_cast<unsigned>(i)
        );
        zassert_equal(
            b.selector_value.index(), a.selector_value.index(), "trigger %u selector kind", static_cast<unsigned>(i)
        );
        zassert_true(b.selector_value == a.selector_value, "trigger %u selector", static_cast<unsigned>(i));
    }
}

} // namespace

ZTEST(logging_control_configuration_parser, test_TriggersRoundTripThroughCbor) {
    auto text = MakeTrigger(0, 1, 0);
    text.binding.selector_value = std::pmr::string("ignition");
    auto number = MakeTrigger(300, 70000, 2);
    number.binding.channel = EventChannelId::Settings;
    number.binding.selector_value = -7;
    auto flag = MakeTrigger(1, 2, 3);
    flag.binding.channel = EventChannelId::Logging;
    flag.binding.selector_value = true;
    auto none = MakeTrigger(0xFFFFFFFF, 0xFFFFFFFF, 9);
    none.binding.channel = EventChannelId::Navigation;
    text.target_uid = 0x12345678;
    number.target_uid = 1;
    none.target_uid = 0xFFFFFFFF;

    const auto configuration = MakeConfiguration({ text, number, flag, none });

    const auto bytes = Encode(configuration);
    zassert_false(bytes.empty(), "The configuration must encode");

    const auto decoded = Decode(bytes);
    zassert_not_null(decoded.get(), "and decode");

    LoggingControlConfigurationCborParser parser;
    const auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded);

    AssertSame(configuration, *restored);
    zassert_true(
        std::holds_alternative<std::monostate>(restored->triggers[3].binding.selector_value),
        "A trigger without a selector has none"
    );
}

ZTEST(logging_control_configuration_parser, test_AnEmptyConfigurationRoundTrips) {
    const auto configuration = MakeConfiguration({});

    const auto bytes = Encode(configuration);
    zassert_true(bytes == K_EMPTY);

    LoggingControlConfigurationCborParser parser;
    const auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), *Decode(bytes));
    zassert_true(restored->triggers.empty());
}

ZTEST(logging_control_configuration_parser, test_TheEncodingIsStable) {
    auto with_selector = MakeTrigger();
    with_selector.binding.selector_value = std::pmr::string("ab");
    with_selector.target_uid = 0x01020304;
    zassert_true(Encode(MakeConfiguration({ with_selector })) == TEXT_SELECTOR, "A text selector");

    zassert_true(Encode(MakeConfiguration({ MakeTrigger() })) == NO_SELECTOR, "No selector is int 0");

    // And the same bytes read back.
    LoggingControlConfigurationCborParser parser;

    const auto text = parser.Deserialize(Mrm::GetDefaultPmr(), *Decode(TEXT_SELECTOR));
    AssertSame(MakeConfiguration({ with_selector }), *text);

    const auto none = parser.Deserialize(Mrm::GetDefaultPmr(), *Decode(NO_SELECTOR));
    AssertSame(MakeConfiguration({ MakeTrigger() }), *none);
}

ZTEST(logging_control_configuration_parser, test_AnAbsentSelectorIsIgnoredWhateverIsStored) {
    // has_selector false with a text value: the value is dropped.
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF4, 0x00, 0x62, 0x61, 0x62, 0x00, 0xFF, 0xFF, 0xFF };

    LoggingControlConfigurationCborParser parser;
    const auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), *Decode(bytes));

    zassert_equal(restored->triggers.size(), 1);
    zassert_true(std::holds_alternative<std::monostate>(restored->triggers[0].binding.selector_value));
}

ZTEST(logging_control_configuration_parser, test_ATriggerWithoutAFieldDoesNotDecode) {
    // The golden bytes without selector_value: every field is required.
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0xFF, 0xFF, 0xFF };

    zassert_is_null(Decode(bytes).get());
}

ZTEST(logging_control_configuration_parser, test_ATriggerWithoutATargetDoesNotDecode) {
    // The layout before target_uid: six fields. Every field is required, so a file stored then is refused.
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF4, 0x00, 0x00, 0xFF, 0xFF, 0xFF };

    zassert_is_null(Decode(bytes).get());
}

ZTEST(logging_control_configuration_parser, test_AFloatSelectorDoesNotDecode) {
    // selector_value is a float64 (1.0): int / tstr / bool only.
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0xFB, 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };

    zassert_is_null(Decode(bytes).get());
}

ZTEST(logging_control_configuration_parser, test_AListSelectorDoesNotDecode) {
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0x82, 0x01, 0x02, 0x00, 0xFF, 0xFF, 0xFF };

    zassert_is_null(Decode(bytes).get());
}

ZTEST(logging_control_configuration_parser, test_TheSizeCoversTheEncoding) {
    // The worst case the validator lets through: the most triggers, each with the longest id and
    // numbers that need every byte. The buffer is as long as the size calculator says.
    std::vector<LoggingTrigger> triggers;
    for(size_t i = 0; i < LoggingControlConfiguration::kMaxTriggers; i++) {
        auto trigger = MakeTrigger(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
        trigger.binding.selector_value = std::pmr::string(64, static_cast<char>('a' + i));
        trigger.target_uid = 0xFFFFFFFF;
        triggers.push_back(std::move(trigger));
    }
    const auto configuration = MakeConfiguration(triggers);

    const auto bytes = Encode(configuration);
    zassert_false(bytes.empty(), "The size calculator must not undercount");

    LoggingControlConfigurationCborParser parser;
    const auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), *Decode(bytes));
    AssertSame(configuration, *restored);

    // And with negative numbers and bools, which have their own sizes.
    auto number = MakeTrigger();
    number.binding.selector_value = -100000;
    auto flag = MakeTrigger();
    flag.binding.selector_value = true;
    zassert_false(Encode(MakeConfiguration({ number, flag })).empty());
}

ZTEST(logging_control_configuration_parser, test_MoreTriggersThanTheSchemaAllowsDoNotDecode) {
    // 0*16: the sixteenth decodes, the seventeenth does not.
    const Bytes trigger = { 0x9F, 0x01, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x00, 0xFF };

    const auto build = [&trigger](size_t count) {
        Bytes bytes = { 0x9F, 0x9F };
        for(size_t i = 0; i < count; i++)
            bytes.insert(bytes.end(), trigger.begin(), trigger.end());
        bytes.insert(bytes.end(), { 0xFF, 0xFF });

        return bytes;
    };

    const auto sixteen = Decode(build(16));
    zassert_not_null(sixteen.get());
    zassert_equal(sixteen->CborLoggingTriggerConfig_m.size(), 16);

    zassert_is_null(Decode(build(17)).get());
}

ZTEST(logging_control_configuration_parser, test_DeserializeValidates) {
    // Channel 0 is no channel.
    const Bytes bytes = { 0x9F, 0x9F, 0x9F, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF };

    const auto decoded = Decode(bytes);
    zassert_not_null(decoded.get(), "The bytes are the schema's");

    LoggingControlConfigurationCborParser parser;
    bool is_rejected = false;
    try {
        parser.Deserialize(Mrm::GetDefaultPmr(), *decoded);
    } catch(const std::invalid_argument&) {
        is_rejected = true;
    }

    zassert_true(is_rejected, "but the configuration they describe is not valid");
}

ZTEST(logging_control_configuration_parser, test_SerializeValidates) {
    LoggingControlConfigurationCborParser parser;

    bool is_rejected = false;
    try {
        parser.Serialize(MakeConfiguration({ LoggingTrigger{} }));
    } catch(const std::invalid_argument&) {
        is_rejected = true;
    }

    zassert_true(is_rejected, "A trigger without a channel is not stored");
}
