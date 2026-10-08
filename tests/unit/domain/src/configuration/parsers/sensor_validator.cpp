#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"
#include "utilities/voltage_interpolator/interpolation_method.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/configuration/parsers/sensor_validator.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorValidator;

ZTEST_SUITE(sensor_validator, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint32_t kChannelCount = 8;

std::shared_ptr<Sensor> MakeSensor(std::string_view id, SensorType type, std::optional<int> sampling_rate_ms) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = type;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;

    switch(type) {
    case SensorType::PHYSICAL_ANALOG:
        sensor->configuration.channel = 0;
        sensor->configuration.interpolation_method = InterpolationMethod::LINEAR;
        sensor->configuration.calibration_table = { {0.0F, 0.0F}, {3.3F, 100.0F} };
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
        SensorValidator::Validate(sensors, nullptr, kChannelCount, kChannelCount);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return std::nullopt;
}

bool Validates(SensorType type, std::optional<int> sampling_rate_ms = 100) {
    return !Validate({ MakeSensor("sensor_1", type, sampling_rate_ms) }).has_value();
}

bool Contains(const std::optional<std::string>& error, std::string_view text) {
    return error.has_value() && error->find(text) != std::string::npos;
}

} // namespace

ZTEST(sensor_validator, test_user_sensor_types_are_valid) {
    zassert_true(Validates(SensorType::USER_ANALOG));
    zassert_true(Validates(SensorType::USER_INDICATOR));
}

ZTEST(sensor_validator, test_none_sentinel_and_unknown_sensor_types_are_invalid) {
    for(auto type : { SensorType::NONE, SensorType::COUNT,
                      static_cast<SensorType>(9999), static_cast<SensorType>(UINT32_MAX) })
        zassert_false(Validates(type), "Accepted invalid sensor type %u.", static_cast<unsigned>(type));
}

ZTEST(sensor_validator, test_every_type_validates_with_its_update_method) {
    zassert_true(Validates(SensorType::PHYSICAL_ANALOG, 100));
    zassert_true(Validates(SensorType::PHYSICAL_INDICATOR, 100));
    zassert_true(Validates(SensorType::PHYSICAL_INDICATOR, std::nullopt), "GPIO edges need no sampling rate");
    zassert_true(Validates(SensorType::VIRTUAL_ANALOG, 100));
    zassert_true(Validates(SensorType::VIRTUAL_INDICATOR, 100));

    // Without a sampling rate a virtual sensor is evaluated when an input commits.
    auto dependent = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, std::nullopt);
    dependent->configuration.expression = "other_sensor * 2";
    zassert_false(Validate({ dependent }).has_value());
    zassert_true(Validates(SensorType::CANBUS_RAW, std::nullopt));
    zassert_true(Validates(SensorType::CANBUS_ANALOG, std::nullopt));
    zassert_true(Validates(SensorType::CANBUS_INDICATOR, std::nullopt));
}

ZTEST(sensor_validator, test_sensor_that_would_never_update_is_rejected) {
    // Polled sensors without a sampling rate resolve to no update method at all; a virtual sensor
    // without one needs an input to be evaluated after ("1 + 1" has none).
    for(auto type : { SensorType::PHYSICAL_ANALOG, SensorType::VIRTUAL_ANALOG, SensorType::VIRTUAL_INDICATOR,
                      SensorType::USER_ANALOG, SensorType::USER_INDICATOR }) {
        auto error = Validate({ MakeSensor("sensor_1", type, std::nullopt) });
        zassert_true(Contains(error, "must have a sampling rate"), "Accepted type %u without a sampling rate.", static_cast<unsigned>(type));
    }

    // CAN bus sensors with one would be scheduled, and the scheduler has no CAN reader.
    for(auto type : { SensorType::CANBUS_RAW, SensorType::CANBUS_ANALOG, SensorType::CANBUS_INDICATOR }) {
        auto error = Validate({ MakeSensor("sensor_1", type, 100) });
        zassert_true(Contains(error, "do not support a sampling rate"), "Accepted CAN type %u with a sampling rate.", static_cast<unsigned>(type));
    }
}

ZTEST(sensor_validator, test_invalid_sampling_rate_is_rejected) {
    zassert_true(Contains(Validate({ MakeSensor("sensor_1", SensorType::PHYSICAL_INDICATOR, 0) }), "Invalid sampling rate"));
    zassert_true(Contains(Validate({ MakeSensor("sensor_1", SensorType::PHYSICAL_INDICATOR, -5) }), "Invalid sampling rate"));
}

ZTEST(sensor_validator, test_duplicate_sensor_ids_are_rejected) {
    auto error = Validate({
        MakeSensor("sensor_1", SensorType::USER_ANALOG, 100),
        MakeSensor("sensor_2", SensorType::USER_ANALOG, 100),
        MakeSensor("sensor_1", SensorType::USER_INDICATOR, 100) });

    zassert_true(Contains(error, "Duplicate sensor ID"));
    zassert_true(Contains(error, "Sensor ID: sensor_1"), "The message names the sensor");
}

ZTEST(sensor_validator, test_sensor_id_hash_collisions_are_rejected) {
    // Readings are keyed by the 32-bit hash; these two IDs collide on it.
    constexpr std::string_view first = "sensor_28093";
    constexpr std::string_view second = "sensor_39328";
    zassert_equal(StringHelpers::GetHash(first), StringHelpers::GetHash(second), "Test IDs must collide");

    auto error = Validate({
        MakeSensor(first, SensorType::USER_ANALOG, 100),
        MakeSensor(second, SensorType::USER_ANALOG, 100) });

    zassert_true(Contains(error, "hash collides"));
    zassert_true(Contains(error, first));
    zassert_true(Contains(error, second));
}

ZTEST(sensor_validator, test_invalid_sensor_id_message_names_the_sensor) {
    zassert_true(Contains(Validate({ MakeSensor("1_sensor", SensorType::USER_ANALOG, 100) }), "Sensor ID: 1_sensor"));
    zassert_true(Contains(Validate({ MakeSensor("sen#sor", SensorType::USER_ANALOG, 100) }), "Sensor ID: sen#sor"));
    zassert_true(Contains(Validate({ MakeSensor("", SensorType::USER_ANALOG, 100) }), "cannot be empty"));
}

ZTEST(sensor_validator, test_metadata_error_names_the_sensor) {
    auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->metadata.name.assign(33, 'n');

    auto error = Validate({ sensor });
    zassert_true(Contains(error, "Sensor name"));
    zassert_true(Contains(error, "Sensor ID: sensor_1"));
}

ZTEST(sensor_validator, test_sensor_count_limit) {
    std::vector<std::shared_ptr<Sensor>> sensors;
    for(int i = 0; i < CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT; i++)
        sensors.push_back(MakeSensor("sensor_" + std::to_string(i), SensorType::USER_ANALOG, 100));

    zassert_false(Validate(sensors).has_value(), "The limit itself is allowed");

    sensors.push_back(MakeSensor("one_too_many", SensorType::USER_ANALOG, 100));

    zassert_true(Contains(Validate(sensors), "at most"));
}

ZTEST(sensor_validator, test_invalid_expression_is_rejected_with_its_message) {
    auto sensor = MakeSensor("sensor_1", SensorType::USER_ANALOG, 100);
    sensor->configuration.expression = "x +";

    auto error = Validate({ sensor });
    zassert_true(Contains(error, "Sensor ID: sensor_1"));
    zassert_true(Contains(error, "Invalid expression"));
}

ZTEST(sensor_validator, test_expression_rules_follow_the_type) {
    auto raw = MakeSensor("raw", SensorType::CANBUS_RAW, std::nullopt);
    raw->configuration.expression = "x * 2";
    zassert_true(Contains(Validate({ raw }), "does not support expression"));

    auto virtual_sensor = MakeSensor("derived", SensorType::VIRTUAL_ANALOG, 100);
    virtual_sensor->configuration.expression.clear();
    zassert_true(Contains(Validate({ virtual_sensor }), "must have expression"));
}

ZTEST(sensor_validator, test_interpolation_rules_follow_the_type) {
    auto analog = MakeSensor("analog", SensorType::PHYSICAL_ANALOG, 100);
    analog->configuration.calibration_table = { {0.0F, 0.0F} };
    zassert_true(Contains(Validate({ analog }), "at least 2 points"));

    analog->configuration.interpolation_method = InterpolationMethod::NONE;
    analog->configuration.calibration_table.clear();
    zassert_true(Contains(Validate({ analog }), "must have interpolation"));

    auto indicator = MakeSensor("indicator", SensorType::PHYSICAL_INDICATOR, std::nullopt);
    indicator->configuration.interpolation_method = InterpolationMethod::LINEAR;
    indicator->configuration.calibration_table = { {0.0F, 0.0F}, {1.0F, 1.0F} };
    zassert_true(Contains(Validate({ indicator }), "does not support interpolation"));
}
