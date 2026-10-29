#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/sensor_domain/configuration/parsers/sensor_validator.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "script_card.h"

using namespace eerie_memory;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorValidator;
using eerie_leap::tests::ScriptCard;

ZTEST_SUITE(sensor_validator, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint32_t K_CHANNEL_COUNT = 8;

std::shared_ptr<Sensor> MakeSensor(std::string_view id, SensorType type, std::optional<int> sampling_rate_ms) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = type;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;

    switch(type) {
        case SensorType::PHYSICAL_ANALOG:
            sensor->configuration.channel = 0;
            sensor->configuration.interpolation_method = InterpolationMethod::LINEAR;
            sensor->configuration.calibration_table = { { 0.0F, 0.0F }, { 3.3F, 100.0F } };
            break;
        case SensorType::PHYSICAL_INDICATOR:
            sensor->configuration.channel = 0;
            break;
        case SensorType::VIRTUAL_ANALOG:
        case SensorType::VIRTUAL_INDICATOR:
            sensor->configuration.expression = "1 + 1";
            break;
        case SensorType::CANBUS_RAW:
            sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 0, 100);
            sensor->configuration.UpdateConnectionString();
            break;
        case SensorType::CANBUS_ANALOG:
        case SensorType::CANBUS_INDICATOR:
            sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 0, 100, "signal");
            sensor->configuration.UpdateConnectionString();
            break;
        default:
            break;
    }

    return sensor;
}

// Returns the validation error, or nothing when the sensors validate.
std::optional<std::string> Validate(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    try {
        SensorValidator::Validate(sensors, nullptr, K_CHANNEL_COUNT, K_CHANNEL_COUNT);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return std::nullopt;
}

bool Validates(SensorType type, std::optional<int> sampling_rate_ms = 100) {
    return !Validate({ MakeSensor("sensor_1", type, sampling_rate_ms) }).has_value();
}

bool Contains(const std::optional<std::string>& error, std::string_view text) {
    return error.has_value() && error->contains(text);
}

// The validation error on a unit with these channel counts, or nothing when the sensors validate.
std::optional<std::string> ValidateOn(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    uint32_t gpio_channel_count,
    uint32_t adc_channel_count
) {

    try {
        SensorValidator::Validate(sensors, nullptr, gpio_channel_count, adc_channel_count);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return std::nullopt;
}

void AssertRejection(const std::optional<std::string>& error, std::string_view expected) {
    zassert_true(error.has_value(), "Accepted, expected: %s", std::string(expected).c_str());
    zassert_true(*error == expected, "Got: %s\nExpected: %s", error->c_str(), std::string(expected).c_str());
}

std::string Rejected(std::string_view sensor_id, std::string_view message) {
    return "Invalid Sensor configuration. Sensor ID: " + std::string(sensor_id) + ". " + std::string(message);
}

// A CAN sensor with its connection string as written, which is what the validator reads.
std::shared_ptr<Sensor> MakeCanSensor(SensorType type, std::string_view connection_string) {
    auto sensor = MakeSensor("can_1", type, std::nullopt);
    sensor->configuration.connection_string = connection_string;

    return sensor;
}

std::shared_ptr<Sensor>
MakeCanSensor(SensorType type, uint8_t bus_channel, uint32_t frame_id, std::string_view signal_name) {
    auto sensor = MakeSensor("can_1", type, std::nullopt);
    sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(
        Mrm::GetDefaultPmr(), bus_channel, frame_id, signal_name
    );
    sensor->configuration.UpdateConnectionString();

    return sensor;
}

// Bus 0 with message 0x7E0, which has the signal rpm.
CanbusConfiguration MakeCanbusConfiguration() {
    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.bus_channel = 0;

    const auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = 0x7E0;

    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.size_bits = 16;
    signal.SetName("rpm");
    message->signal_configurations.emplace_back(std::move(signal));

    channel.message_configurations.emplace_back(message);
    configuration.channel_configurations.emplace(0, std::move(channel));

    return configuration;
}

std::optional<std::string> CanbusSourcesRejection(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    const CanbusConfiguration& canbus_configuration
) {

    try {
        SensorValidator::ValidateCanbusSources(sensors, canbus_configuration);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return std::nullopt;
}

std::shared_ptr<Sensor> Disabled(std::shared_ptr<Sensor> sensor) {
    sensor->is_enabled = false;

    return sensor;
}

} // namespace

ZTEST(sensor_validator, test_user_sensor_types_are_valid) {
    zassert_true(Validates(SensorType::USER_ANALOG));
    zassert_true(Validates(SensorType::USER_INDICATOR));
}

ZTEST(sensor_validator, test_none_sentinel_and_unknown_sensor_types_are_invalid) {
    for(const auto type :
        { SensorType::NONE, SensorType::COUNT, static_cast<SensorType>(9999), static_cast<SensorType>(UINT32_MAX) })
        zassert_false(Validates(type), "Accepted invalid sensor type %u.", static_cast<unsigned>(type));
}

ZTEST(sensor_validator, test_every_type_validates_with_its_update_method) {
    zassert_true(Validates(SensorType::PHYSICAL_ANALOG, 100));
    zassert_true(Validates(SensorType::PHYSICAL_INDICATOR, 100));
    zassert_true(Validates(SensorType::PHYSICAL_INDICATOR, std::nullopt), "GPIO edges need no sampling rate");
    zassert_true(Validates(SensorType::VIRTUAL_ANALOG, 100));
    zassert_true(Validates(SensorType::VIRTUAL_INDICATOR, 100));

    // Without a sampling rate a virtual sensor is evaluated when an input commits.
    const auto dependent = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt);
    dependent->configuration.expression = "other_sensor * 2";
    zassert_false(Validate({ MakeSensor("other_sensor", SensorType::PHYSICAL_ANALOG, 100), dependent }).has_value());
    zassert_true(Validates(SensorType::CANBUS_RAW, std::nullopt));
    zassert_true(Validates(SensorType::CANBUS_ANALOG, std::nullopt));
    zassert_true(Validates(SensorType::CANBUS_INDICATOR, std::nullopt));
    zassert_true(Validates(SensorType::CONTROL_INDICATOR, std::nullopt), "A control's value needs no sampling rate");
    zassert_true(Validates(SensorType::CONTROL_ANALOG, std::nullopt));
}

ZTEST(sensor_validator, test_control_sensors_take_no_sampling_rate_channel_or_connection_string) {
    for(const auto type : { SensorType::CONTROL_INDICATOR, SensorType::CONTROL_ANALOG }) {
        AssertRejection(
            Validate({ MakeSensor("control_1", type, 100) }),
            Rejected(
                "control_1",
                "CAN bus and control sensors are updated by their source and do not support a sampling rate."
            )
        );

        const auto with_channel = MakeSensor("control_1", type, std::nullopt);
        with_channel->configuration.channel = 0;
        AssertRejection(
            Validate({ with_channel }), Rejected("control_1", "Channel value is not supported for this sensor type.")
        );

        const auto with_connection_string = MakeSensor("control_1", type, std::nullopt);
        with_connection_string->configuration.connection_string = "0/100/rpm";
        AssertRejection(
            Validate({ with_connection_string }),
            Rejected("control_1", "Connection string is not supported for this sensor type.")
        );
    }
}

ZTEST(sensor_validator, test_sensor_that_would_never_update_is_rejected) {
    // Polled sensors without a sampling rate resolve to no update method at all; a virtual sensor
    // without one needs an input to be evaluated after ("1 + 1" has none).
    for(const auto type : { SensorType::PHYSICAL_ANALOG, SensorType::VIRTUAL_ANALOG, SensorType::VIRTUAL_INDICATOR,
            SensorType::USER_ANALOG, SensorType::USER_INDICATOR }) {
        const auto error = Validate({ MakeSensor("sensor_1", type, std::nullopt) });
        zassert_true(
            Contains(error, "must have a sampling rate"), "Accepted type %u without a sampling rate.",
            static_cast<unsigned>(type)
        );
    }

    // CAN bus sensors with one would be scheduled, and the scheduler has no CAN reader.
    for(const auto type : { SensorType::CANBUS_RAW, SensorType::CANBUS_ANALOG, SensorType::CANBUS_INDICATOR }) {
        const auto error = Validate({ MakeSensor("sensor_1", type, 100) });
        zassert_true(
            Contains(error, "do not support a sampling rate"), "Accepted CAN type %u with a sampling rate.",
            static_cast<unsigned>(type)
        );
    }
}

ZTEST(sensor_validator, test_invalid_sampling_rate_is_rejected) {
    zassert_true(
        Contains(Validate({ MakeSensor("sensor_1", SensorType::PHYSICAL_INDICATOR, 0) }), "Invalid sampling rate")
    );
    zassert_true(
        Contains(Validate({ MakeSensor("sensor_1", SensorType::PHYSICAL_INDICATOR, -5) }), "Invalid sampling rate")
    );
}

ZTEST(sensor_validator, test_duplicate_sensor_ids_are_rejected) {
    const auto error = Validate(
        { MakeSensor("sensor_1", SensorType::USER_ANALOG, 100), MakeSensor("sensor_2", SensorType::USER_ANALOG, 100),
            MakeSensor("sensor_1", SensorType::USER_INDICATOR, 100) }
    );

    zassert_true(Contains(error, "Duplicate sensor ID"));
    zassert_true(Contains(error, "Sensor ID: sensor_1"), "The message names the sensor");
}

ZTEST(sensor_validator, test_sensor_id_hash_collisions_are_rejected) {
    // Readings are keyed by the 32-bit hash; these two IDs collide on it.
    constexpr std::string_view FIRST = "sensor_28093";
    constexpr std::string_view SECOND = "sensor_39328";
    zassert_equal(StringHelpers::GetHash(FIRST), StringHelpers::GetHash(SECOND), "Test IDs must collide");

    const auto error = Validate(
        { MakeSensor(FIRST, SensorType::USER_ANALOG, 100), MakeSensor(SECOND, SensorType::USER_ANALOG, 100) }
    );

    zassert_true(Contains(error, "hash collides"));
    zassert_true(Contains(error, FIRST));
    zassert_true(Contains(error, SECOND));
}

ZTEST(sensor_validator, test_invalid_sensor_id_message_names_the_sensor) {
    zassert_true(Contains(Validate({ MakeSensor("1_sensor", SensorType::USER_ANALOG, 100) }), "Sensor ID: 1_sensor"));
    zassert_true(Contains(Validate({ MakeSensor("sen#sor", SensorType::USER_ANALOG, 100) }), "Sensor ID: sen#sor"));
    zassert_true(Contains(Validate({ MakeSensor("", SensorType::USER_ANALOG, 100) }), "cannot be empty"));
}

ZTEST(sensor_validator, test_metadata_limits_are_in_bytes_and_include_the_limit) {
    const auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->metadata.name.assign(32, 'n');
    sensor->metadata.unit.assign(32, 'u');
    sensor->metadata.description.assign(128, 'd');
    zassert_false(Validate({ sensor }).has_value(), "The limits themselves are allowed");

    // 17 two-byte characters are 34 bytes.
    sensor->metadata.name.clear();
    for(int i = 0; i < 17; i++)
        sensor->metadata.name += "\xC3\xA9";
    AssertRejection(
        Validate({ sensor }),
        "Invalid Sensor metadata configuration. Sensor ID: sensor_1. Sensor name must be at most 32 bytes."
    );

    sensor->metadata.name.assign(32, 'n');
    sensor->metadata.unit.assign(33, 'u');
    AssertRejection(
        Validate({ sensor }),
        "Invalid Sensor metadata configuration. Sensor ID: sensor_1. Sensor unit must be at most 32 bytes."
    );

    sensor->metadata.unit.assign(32, 'u');
    sensor->metadata.description.assign(129, 'd');
    AssertRejection(
        Validate({ sensor }),
        "Invalid Sensor metadata configuration. Sensor ID: sensor_1. Sensor description must be at most 128 bytes."
    );
}

ZTEST(sensor_validator, test_channel_out_of_range_names_the_channels_of_the_unit) {
    const auto analog = MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100);

    analog->configuration.channel = 4;
    AssertRejection(
        ValidateOn({ analog }, 8, 4),
        Rejected("analog", "Channel value is out of range. This unit has ADC channels 0 to 3.")
    );

    analog->configuration.channel = 1;
    AssertRejection(
        ValidateOn({ analog }, 8, 1),
        Rejected("analog", "Channel value is out of range. This unit has only ADC channel 0.")
    );

    analog->configuration.channel = 0;
    AssertRejection(
        ValidateOn({ analog }, 8, 0),
        Rejected("analog", "Channel value is out of range. This unit has no ADC channels.")
    );

    const auto indicator = MakeSensor("indicator", SensorType::PHYSICAL_INDICATOR, std::nullopt);
    indicator->configuration.channel = 8;
    AssertRejection(
        ValidateOn({ indicator }, 8, 4),
        Rejected("indicator", "Channel value is out of range. This unit has GPIO channels 0 to 7.")
    );

    indicator->configuration.channel = 7;
    zassert_false(ValidateOn({ indicator }, 8, 4).has_value(), "The last channel is in range");
}

ZTEST(sensor_validator, test_connection_string_is_read_strictly) {
    for(const char* text :
        { "1/0010/rpm", "1/0x7E0/rpm", "1/0X7e0/rpm", "255/536870911/rpm", "0/0x1FFFFFFF/rpm", "1/100/a/b" })
        zassert_false(Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, text) }).has_value(), "Rejected %s", text);
    zassert_false(
        Validate({ MakeCanSensor(SensorType::CANBUS_RAW, "1/100") }).has_value(), "A raw sensor has no signal"
    );

    AssertRejection(
        Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, "") }),
        Rejected("can_1", "Connection string cannot be empty.")
    );

    for(const char* text : { "1", "1/100/" }) {
        AssertRejection(
            Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, text) }),
            Rejected("can_1", "Connection string must be bus_channel/frame_id or bus_channel/frame_id/signal_name.")
        );
    }

    for(const char* text : { "256/1/rpm", "-1/1/rpm", "+1/1/rpm", " 1/1/rpm", "0x/1/rpm", "/1/rpm", "1a/1/rpm" }) {
        AssertRejection(
            Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, text) }),
            Rejected("can_1", "Connection string bus channel must be a number from 0 to 255.")
        );
    }

    for(const char* text :
        { "1/536870912/rpm", "1/0x20000000/rpm", "1/12abc/rpm", "1//rpm", "1/99999999999999999999/rpm", "1/0x/rpm" }) {
        AssertRejection(
            Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, text) }),
            Rejected("can_1", "Connection string frame ID must be a number from 0 to 0x1FFFFFFF.")
        );
    }

    AssertRejection(
        Validate({ MakeCanSensor(SensorType::CANBUS_ANALOG, "1/100") }),
        Rejected("can_1", "Sensor must have CAN bus signal name.")
    );
}

ZTEST(sensor_validator, test_calibration_table_holds_at_most_50_finite_points_with_distinct_voltages) {
    const auto analog = MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100);
    auto& calibration_table = analog->configuration.calibration_table;

    calibration_table.clear();
    for(size_t i = 0; i < SensorValidator::MAX_CALIBRATION_POINTS; i++)
        calibration_table.push_back({ .voltage = static_cast<float>(i) * 0.1F, .value = static_cast<float>(i) });
    zassert_false(Validate({ analog }).has_value(), "50 points are allowed");

    calibration_table.push_back({ .voltage = 10.0F, .value = 50.0F });
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration table must have at most 50 points."));

    constexpr float NAN = std::numeric_limits<float>::quiet_NaN();
    constexpr float INFINITY = std::numeric_limits<float>::infinity();

    calibration_table = { { 0.0F, 0.0F }, { NAN, 1.0F } };
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration table must hold finite numbers."));

    calibration_table = { { 0.0F, 0.0F }, { 1.0F, -INFINITY } };
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration table must hold finite numbers."));

    // A repeated voltage need not be next to the first one: the table is ordered only after validation.
    calibration_table = { { 0.0F, 0.0F }, { 3.0F, 1.0F }, { 0.0F, 2.0F } };
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration voltages must be unique."));

    calibration_table = { { 0.0F, 0.0F }, { -0.0F, 1.0F } };
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration voltages must be unique."));

    calibration_table = { { 3.0F, 1.0F }, { 0.0F, 0.0F } };
    zassert_false(Validate({ analog }).has_value(), "An unordered table with distinct voltages is valid");
}

ZTEST(sensor_validator, test_expression_of_a_sensor_without_an_input_cannot_use_x) {
    for(const auto type : { SensorType::VIRTUAL_ANALOG, SensorType::VIRTUAL_INDICATOR }) {
        const auto sensor = MakeSensor("derived", type, 100);
        sensor->configuration.expression = "x + 1";

        AssertRejection(
            Validate({ sensor }), Rejected("derived", "Sensor has no input value, the expression cannot use x.")
        );
    }

    for(const auto type : { SensorType::PHYSICAL_ANALOG, SensorType::PHYSICAL_INDICATOR, SensorType::CANBUS_ANALOG,
            SensorType::CANBUS_INDICATOR, SensorType::USER_ANALOG, SensorType::USER_INDICATOR,
            SensorType::CONTROL_INDICATOR, SensorType::CONTROL_ANALOG }) {
        const auto sampling_rate_ms = SensorTypeTraits::Of(type).allows_sampling_rate ? std::optional<int>(100)
                                                                                      : std::nullopt;
        const auto sensor = MakeSensor("sensor_1", type, sampling_rate_ms);
        sensor->configuration.expression = "x * 2";

        zassert_false(Validate({ sensor }).has_value(), "Rejected x for type %u.", static_cast<unsigned>(type));
    }
}

ZTEST(sensor_validator, test_can_sources_must_name_a_configured_bus_message_and_signal) {
    const auto canbus_configuration = MakeCanbusConfiguration();

    zassert_false(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E0, "rpm") }, canbus_configuration)
            .has_value()
    );
    zassert_false(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_INDICATOR, 0, 0x7E0, "rpm") }, canbus_configuration)
            .has_value()
    );
    zassert_false(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_RAW, 0, 0x123, "") }, canbus_configuration)
            .has_value(),
        "A raw sensor reads any frame on a configured bus"
    );

    AssertRejection(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_ANALOG, 1, 0x7E0, "rpm") }, canbus_configuration),
        Rejected("can_1", "CAN bus 1 is not configured.")
    );
    AssertRejection(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_RAW, 2, 0x7E0, "") }, canbus_configuration),
        Rejected("can_1", "CAN bus 2 is not configured.")
    );
    AssertRejection(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E1, "rpm") }, canbus_configuration),
        Rejected("can_1", "CAN bus 0 has no message with frame ID 0x7E1.")
    );
    AssertRejection(
        CanbusSourcesRejection(
            { MakeCanSensor(SensorType::CANBUS_INDICATOR, 0, 0x7E0, "speed") }, canbus_configuration
        ),
        Rejected("can_1", "CAN message 0x7E0 on bus 0 has no signal speed.")
    );

    const CanbusConfiguration no_channels(std::allocator_arg, Mrm::GetDefaultPmr());
    zassert_false(
        CanbusSourcesRejection({ MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100) }, no_channels).has_value(),
        "Only CAN sensors are checked"
    );
}

ZTEST(sensor_validator, test_metadata_error_names_the_sensor) {
    const auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->metadata.name.assign(33, 'n');

    const auto error = Validate({ sensor });
    zassert_true(Contains(error, "Sensor name"));
    zassert_true(Contains(error, "Sensor ID: sensor_1"));
}

ZTEST(sensor_validator, test_sensor_count_limit) {
    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT);
    for(int i = 0; i < CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT; i++)
        sensors.push_back(MakeSensor("sensor_" + std::to_string(i), SensorType::USER_ANALOG, 100));

    zassert_false(Validate(sensors).has_value(), "The limit itself is allowed");

    sensors.push_back(MakeSensor("one_too_many", SensorType::USER_ANALOG, 100));

    zassert_true(Contains(Validate(sensors), "at most"));
}

ZTEST(sensor_validator, test_invalid_expression_is_rejected_with_its_message) {
    const auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->configuration.expression = "x +";

    const auto error = Validate({ sensor });
    zassert_true(Contains(error, "Sensor ID: sensor_1"));
    zassert_true(Contains(error, "Invalid expression"));
}

ZTEST(sensor_validator, test_expression_referencing_an_unknown_sensor_is_rejected) {
    const auto dependent = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt);
    dependent->configuration.expression = "other_sensor * 2";

    const auto error = Validate({ dependent });
    zassert_true(Contains(error, "Sensor ID: derived"));
    zassert_true(Contains(error, "unknown sensor other_sensor"));
}

ZTEST(sensor_validator, test_expression_rules_follow_the_type) {
    const auto raw = MakeSensor("raw", SensorType::CANBUS_RAW, std::nullopt);
    raw->configuration.expression = "x * 2";
    zassert_true(Contains(Validate({ raw }), "does not support expression"));

    const auto virtual_sensor = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, 100);
    virtual_sensor->configuration.expression.clear();
    zassert_true(Contains(Validate({ virtual_sensor }), "must have expression"));
}

ZTEST(sensor_validator, test_interpolation_rules_follow_the_type) {
    const auto analog = MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100);
    analog->configuration.calibration_table = { { 0.0F, 0.0F } };
    zassert_true(Contains(Validate({ analog }), "at least 2 points"));

    analog->configuration.interpolation_method = InterpolationMethod::NONE;
    analog->configuration.calibration_table.clear();
    zassert_true(Contains(Validate({ analog }), "must have interpolation"));

    const auto indicator = MakeSensor("indicator", SensorType::PHYSICAL_INDICATOR, std::nullopt);
    indicator->configuration.interpolation_method = InterpolationMethod::LINEAR;
    indicator->configuration.calibration_table = { { 0.0F, 0.0F }, { 1.0F, 1.0F } };
    zassert_true(Contains(Validate({ indicator }), "does not support interpolation"));
}

ZTEST(sensor_validator, test_expression_of_an_enabled_sensor_cannot_read_a_disabled_sensor) {
    const auto input = Disabled(MakeSensor("input", SensorType::USER_ANALOG, 100));

    const auto derived = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt);
    derived->configuration.expression = "input * 2";

    // A disabled input never gets a value, so the expression would never be evaluated.
    const auto expected = Rejected("derived", "Expression references disabled sensor input.");
    AssertRejection(Validate({ input, derived }), expected);
    AssertRejection(Validate({ derived, input }), expected);

    derived->configuration.sampling_rate_ms = 100;
    AssertRejection(Validate({ input, derived }), expected);

    // Also next to the sensor's own input value.
    const auto analog = MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100);
    analog->configuration.expression = "x * 2 + input";
    AssertRejection(Validate({ input, analog }), Rejected("analog", "Expression references disabled sensor input."));
}

ZTEST(sensor_validator, test_a_disabled_sensor_may_read_any_sensor) {
    const auto input = MakeSensor("input", SensorType::USER_ANALOG, 100);
    const auto derived = Disabled(MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt));
    derived->configuration.expression = "input * 2";

    zassert_false(Validate({ input, derived }).has_value(), "A disabled sensor reading an enabled one was rejected");

    input->is_enabled = false;
    zassert_false(Validate({ input, derived }).has_value(), "A disabled sensor reading a disabled one was rejected");

    input->is_enabled = true;
    derived->is_enabled = true;
    zassert_false(Validate({ input, derived }).has_value(), "An enabled sensor reading an enabled one was rejected");
}

// A disabled sensor gets every check an enabled one gets, so switching it on can fail only on what
// it references: a disabled sensor or CAN message.
ZTEST(sensor_validator, test_a_disabled_sensor_is_validated_like_an_enabled_one) {
    zassert_true(Contains(
        Validate({ Disabled(MakeSensor("sensor_1", SensorType::PHYSICAL_ANALOG, std::nullopt)) }),
        "must have a sampling rate"
    ));
    zassert_true(Contains(
        Validate({ Disabled(MakeSensor("sensor_1", SensorType::PHYSICAL_INDICATOR, 0)) }), "Invalid sampling rate"
    ));
    zassert_true(
        Contains(Validate({ Disabled(MakeSensor("sensor_1", SensorType::NONE, 100)) }), "Invalid sensor type")
    );

    const auto analog = Disabled(MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100));
    analog->configuration.channel = 4;
    AssertRejection(
        ValidateOn({ analog }, 8, 4),
        Rejected("analog", "Channel value is out of range. This unit has ADC channels 0 to 3.")
    );

    analog->configuration.channel = 0;
    analog->configuration.calibration_table = { { 0.0F, 0.0F }, { 0.0F, 1.0F } };
    AssertRejection(Validate({ analog }), Rejected("analog", "Calibration voltages must be unique."));

    const auto unfinished = Disabled(MakeSensor("sensor_1", SensorType::USER_ANALOG, 100));
    unfinished->configuration.expression = "x +";
    zassert_true(Contains(Validate({ unfinished }), "Invalid expression"));

    const auto unknown = Disabled(MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt));
    unknown->configuration.expression = "other_sensor * 2";
    AssertRejection(Validate({ unknown }), Rejected("derived", "Expression references unknown sensor other_sensor."));

    AssertRejection(
        Validate({ Disabled(MakeCanSensor(SensorType::CANBUS_ANALOG, "1/100")) }),
        Rejected("can_1", "Sensor must have CAN bus signal name.")
    );

    const auto long_name = Disabled(MakeSensor("sensor_1", SensorType::USER_ANALOG, 100));
    long_name->metadata.name.assign(33, 'n');
    zassert_true(Contains(Validate({ long_name }), "Sensor name must be at most 32 bytes"));

    zassert_true(Contains(
        Validate(
            { MakeSensor("sensor_1", SensorType::USER_ANALOG, 100),
                Disabled(MakeSensor("sensor_1", SensorType::USER_ANALOG, 100)) }
        ),
        "Duplicate sensor ID"
    ));
}

// A disabled sensor keeps its slot in the readings frame.
ZTEST(sensor_validator, test_disabled_sensors_count_toward_the_sensor_limit) {
    std::vector<std::shared_ptr<Sensor>> sensors;
    sensors.reserve(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT);
    for(int i = 0; i < CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT; i++)
        sensors.push_back(MakeSensor("sensor_" + std::to_string(i), SensorType::USER_ANALOG, 100));

    sensors.push_back(Disabled(MakeSensor("one_too_many", SensorType::USER_ANALOG, 100)));

    zassert_true(Contains(Validate(sensors), "at most"));
}

ZTEST(sensor_validator, test_an_enabled_can_signal_sensor_cannot_read_a_disabled_message) {
    auto canbus_configuration = MakeCanbusConfiguration();
    canbus_configuration.channel_configurations.at(0).message_configurations.front()->is_enabled = false;

    // The frame router treats a disabled message as absent, so the sensor would never read.
    for(const auto type : { SensorType::CANBUS_ANALOG, SensorType::CANBUS_INDICATOR }) {
        AssertRejection(
            CanbusSourcesRejection({ MakeCanSensor(type, 0, 0x7E0, "rpm") }, canbus_configuration),
            Rejected("can_1", "CAN message 0x7E0 on bus 0 is disabled.")
        );
    }

    // After the message is found and before its signals are.
    AssertRejection(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E1, "rpm") }, canbus_configuration),
        Rejected("can_1", "CAN bus 0 has no message with frame ID 0x7E1.")
    );
    AssertRejection(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E0, "speed") }, canbus_configuration),
        Rejected("can_1", "CAN message 0x7E0 on bus 0 is disabled.")
    );

    zassert_false(
        CanbusSourcesRejection({ MakeCanSensor(SensorType::CANBUS_RAW, 0, 0x7E0, "") }, canbus_configuration)
            .has_value(),
        "A raw sensor reads frames by ID and needs no message definition"
    );
}

ZTEST(sensor_validator, test_a_disabled_can_signal_sensor_may_read_a_disabled_message) {
    auto canbus_configuration = MakeCanbusConfiguration();
    canbus_configuration.channel_configurations.at(0).message_configurations.front()->is_enabled = false;

    const auto sensor = Disabled(MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E0, "rpm"));
    zassert_false(CanbusSourcesRejection({ sensor }, canbus_configuration).has_value());

    // It still gets the checks an enabled sensor gets.
    AssertRejection(
        CanbusSourcesRejection(
            { Disabled(MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E0, "speed")) }, canbus_configuration
        ),
        Rejected("can_1", "CAN message 0x7E0 on bus 0 has no signal speed.")
    );
    AssertRejection(
        CanbusSourcesRejection(
            { Disabled(MakeCanSensor(SensorType::CANBUS_ANALOG, 0, 0x7E1, "rpm")) }, canbus_configuration
        ),
        Rejected("can_1", "CAN bus 0 has no message with frame ID 0x7E1.")
    );
    AssertRejection(
        CanbusSourcesRejection(
            { Disabled(MakeCanSensor(SensorType::CANBUS_INDICATOR, 1, 0x7E0, "rpm")) }, canbus_configuration
        ),
        Rejected("can_1", "CAN bus 1 is not configured.")
    );
}

namespace {

// A sensor of @p type that validates on its own, with a sampling rate only where the type allows one.
std::shared_ptr<Sensor> MakeValidSensor(SensorType type) {
    return MakeSensor(
        "sensor_1", type, SensorTypeTraits::Of(type).allows_sampling_rate ? std::optional<int>(100) : std::nullopt
    );
}

std::shared_ptr<Sensor>
Conditioned(std::shared_ptr<Sensor> sensor, uint32_t debounce_ms, IndicatorInputMode input_mode) {
    sensor->configuration.debounce_ms = debounce_ms;
    sensor->configuration.input_mode = input_mode;

    return sensor;
}

} // namespace

ZTEST(sensor_validator, test_conditioning_is_accepted_on_every_indicator_type) {
    for(const auto type : { SensorType::PHYSICAL_INDICATOR, SensorType::VIRTUAL_INDICATOR, SensorType::CANBUS_INDICATOR,
            SensorType::USER_INDICATOR, SensorType::CONTROL_INDICATOR }) {
        const auto error = Validate({ Conditioned(MakeValidSensor(type), 30, IndicatorInputMode::Toggle) });
        zassert_false(error.has_value(), "Type %u: %s", static_cast<unsigned>(type), error.value_or("").c_str());
    }
}

ZTEST(sensor_validator, test_conditioning_on_a_type_that_is_not_an_indicator_is_rejected) {
    for(const auto type : { SensorType::PHYSICAL_ANALOG, SensorType::VIRTUAL_ANALOG, SensorType::CANBUS_RAW,
            SensorType::CANBUS_ANALOG, SensorType::USER_ANALOG, SensorType::CONTROL_ANALOG }) {
        const auto debounced = Conditioned(MakeValidSensor(type), 1, IndicatorInputMode::Level);
        AssertRejection(
            Validate({ debounced }), Rejected("sensor_1", "Debounce is only supported for indicator sensors.")
        );

        const auto toggled = Conditioned(MakeValidSensor(type), 0, IndicatorInputMode::Toggle);
        AssertRejection(
            Validate({ toggled }), Rejected("sensor_1", "Input mode is only supported for indicator sensors.")
        );

        // A disabled sensor is validated like an enabled one.
        AssertRejection(
            Validate({ Disabled(Conditioned(MakeValidSensor(type), 1, IndicatorInputMode::Level)) }),
            Rejected("sensor_1", "Debounce is only supported for indicator sensors.")
        );
        AssertRejection(
            Validate({ Disabled(Conditioned(MakeValidSensor(type), 0, IndicatorInputMode::Toggle)) }),
            Rejected("sensor_1", "Input mode is only supported for indicator sensors.")
        );

        zassert_false(
            Validate({ MakeValidSensor(type) }).has_value(), "The defaults are valid on type %u",
            static_cast<unsigned>(type)
        );
    }
}

ZTEST(sensor_validator, test_debounce_is_capped_at_60_seconds) {
    zassert_equal(SensorValidator::MAX_DEBOUNCE_MS, 60000);

    const auto at_cap = Conditioned(MakeValidSensor(SensorType::PHYSICAL_INDICATOR), 60000, IndicatorInputMode::Level);
    zassert_false(Validate({ at_cap }).has_value(), "60 s is allowed");

    const auto over_cap = Conditioned(
        MakeValidSensor(SensorType::PHYSICAL_INDICATOR), 60001, IndicatorInputMode::Toggle
    );
    AssertRejection(Validate({ over_cap }), Rejected("sensor_1", "Debounce must be at most 60000 ms."));
}

ZTEST(sensor_validator, test_unknown_input_mode_is_rejected) {
    for(const auto input_mode : { IndicatorInputMode::Count, static_cast<IndicatorInputMode>(UINT32_MAX) }) {
        AssertRejection(
            Validate({ Conditioned(MakeValidSensor(SensorType::PHYSICAL_INDICATOR), 0, input_mode) }),
            Rejected("sensor_1", "Invalid indicator input mode.")
        );

        // Named as unknown before the type is held against it.
        AssertRejection(
            Validate({ Conditioned(MakeValidSensor(SensorType::PHYSICAL_ANALOG), 0, input_mode) }),
            Rejected("sensor_1", "Invalid indicator input mode.")
        );
    }
}

namespace {

// The error with a scripted sensor on this card, or nothing when it validates.
std::optional<std::string> ValidateScriptOn(ScriptCard& card, std::string_view script_path) {
    const auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->configuration.script_path = script_path;

    try {
        SensorValidator::Validate({ sensor }, &card, K_CHANNEL_COUNT, K_CHANNEL_COUNT);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return std::nullopt;
}

} // namespace

ZTEST(sensor_validator, test_a_script_path_must_exist_on_a_mounted_card) {
    ScriptCard card;
    card.files.insert("scripts/sensor_1.lua");

    zassert_false(ValidateScriptOn(card, "scripts/sensor_1.lua").has_value());
    zassert_false(ValidateScriptOn(card, "").has_value(), "A sensor without a script needs no file");
    zassert_true(Contains(ValidateScriptOn(card, "scripts/missing.lua"), "Invalid sensor script path."));
}

// A stored configuration is validated as it loads: rejecting it while the card is out would run the
// unit on defaults.
ZTEST(sensor_validator, test_a_script_path_is_not_checked_while_no_card_is_mounted) {
    ScriptCard card;
    card.is_mounted = false;

    zassert_false(ValidateScriptOn(card, "scripts/sensor_1.lua").has_value());
}
