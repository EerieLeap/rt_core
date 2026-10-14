#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "subsys/expression_engine/expression_evaluator.h"
#include "configuration/cbor/cbor_serializer.h"

#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/models/sensor.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::subsys::expression_engine;
using eerie_leap::configuration::cbor::CborSerializer;

using namespace eerie_leap::domain::sensor_domain::configuration::parsers;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::utilities::voltage_interpolator;

ZTEST_SUITE(sensors_parser, NULL, NULL, NULL, NULL, NULL);

std::vector<std::shared_ptr<Sensor>> sensors_parser_GetTestSensors() {
    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {3.3, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);

    auto sensor_1 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_1");

    sensor_1->metadata.name = "Sensor 1";
    sensor_1->metadata.unit = "km/h";
    sensor_1->metadata.description = "Test Sensor 1";

    sensor_1->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_1->configuration.channel = 0;
    sensor_1->configuration.sampling_rate_ms = 1000;
    sensor_1->configuration.interpolation_method = InterpolationMethod::LINEAR;
    sensor_1->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());
    sensor_1->configuration.expression = "x * 2 + sensor_2 + 1";

    std::pmr::vector<CalibrationData> calibration_data_2 {
        {0.0, 0.0},
        {1.0, 29.0},
        {2.0, 111.0},
        {2.5, 162.0},
        {3.3, 200.0}
    };
    auto calibration_data_2_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_2);

    auto sensor_2 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_2");

    sensor_2->metadata.name = "Sensor 2";
    sensor_2->metadata.unit = "km/h";
    sensor_2->metadata.description = "Test Sensor 2";

    sensor_2->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_2->configuration.channel = 1;
    sensor_2->configuration.sampling_rate_ms = 500;
    sensor_2->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_2->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());
    sensor_2->configuration.expression = "x * 4 + 1.6";

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor_3->configuration.channel = std::nullopt;
    sensor_3->configuration.sampling_rate_ms = 2000;
    sensor_3->configuration.expression = "sensor_1 + 8.34";

    auto sensor_4 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_4");

    sensor_4->metadata.name = "Sensor 4";
    sensor_4->metadata.unit = "km/h";
    sensor_4->metadata.description = "Test Sensor 4";

    sensor_4->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_4->configuration.channel = 2;
    sensor_4->configuration.sampling_rate_ms = 2000;
    sensor_4->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_4->configuration.calibration_table.assign(calibration_data_2_ptr->begin(), calibration_data_2_ptr->end());

    std::vector<std::shared_ptr<Sensor>> sensors = {
        std::move(sensor_1), std::move(sensor_2), std::move(sensor_3), std::move(sensor_4)
    };

    return sensors;
}

void sensors_parser_CompareSensors(
    std::vector<std::shared_ptr<Sensor>>& sensors,
    std::vector<std::shared_ptr<Sensor>>& deserialized_sensors) {

    zassert_equal(deserialized_sensors.size(), sensors.size());

    for(size_t i = 0; i < sensors.size(); ++i) {
        std::shared_ptr<Sensor> deserialized_sensor = nullptr;
        for(size_t j = 0; j < deserialized_sensors.size(); ++j) {
            if(strcmp(deserialized_sensors[j]->id.c_str(), sensors[i]->id.c_str()) == 0) {
                deserialized_sensor = deserialized_sensors[j];
                break;
            }
        }
        zassert_true(deserialized_sensor != nullptr);

        zassert_equal(deserialized_sensor->is_enabled, sensors[i]->is_enabled, "%s", sensors[i]->id.c_str());

        zassert_true(deserialized_sensor->metadata.name == sensors[i]->metadata.name);
        zassert_true(deserialized_sensor->metadata.unit == sensors[i]->metadata.unit);
        zassert_true(deserialized_sensor->metadata.description == sensors[i]->metadata.description);

        zassert_true(deserialized_sensor->configuration.type == sensors[i]->configuration.type);
        if(deserialized_sensor->configuration.channel.has_value() && sensors[i]->configuration.channel.has_value()) {
            zassert_true(deserialized_sensor->configuration.channel.value() == sensors[i]->configuration.channel.value());
        } else {
            zassert_true(!deserialized_sensor->configuration.channel.has_value() && !sensors[i]->configuration.channel.has_value());
        }
        zassert_true(deserialized_sensor->configuration.sampling_rate_ms == sensors[i]->configuration.sampling_rate_ms);
        zassert_true(deserialized_sensor->configuration.interpolation_method == sensors[i]->configuration.interpolation_method);
        zassert_true(deserialized_sensor->configuration.calibration_table.size() == sensors[i]->configuration.calibration_table.size());
        zassert_true(deserialized_sensor->configuration.expression == sensors[i]->configuration.expression);
        zassert_equal(deserialized_sensor->configuration.debounce_ms, sensors[i]->configuration.debounce_ms, "%s", sensors[i]->id.c_str());
        zassert_equal(deserialized_sensor->configuration.input_mode, sensors[i]->configuration.input_mode, "%s", sensors[i]->id.c_str());
    }
}

ZTEST(sensors_parser, test_CborSerializeDeserialize) {
    auto sensors_cbor_parser = std::make_shared<SensorsCborParser>(nullptr);

    auto sensors = sensors_parser_GetTestSensors();

    auto serialized_sensors = sensors_cbor_parser->Serialize(sensors, 16, 16);
    auto deserialized_sensors = sensors_cbor_parser->Deserialize(Mrm::GetDefaultPmr(), *serialized_sensors, 16, 16);

    sensors_parser_CompareSensors(sensors, deserialized_sensors);
}

ZTEST(sensors_parser, test_Deserialize_orders_an_unordered_calibration_table) {
    auto sensors_cbor_parser = std::make_shared<SensorsCborParser>(nullptr);

    auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = sensors_cbor_parser->Serialize(sensors, 16, 16);

    for(auto& sensor_config : serialized_sensors->CborSensorConfig_m)
        std::ranges::reverse(sensor_config.configuration.calibration_table.float32float);

    auto deserialized_sensors = sensors_cbor_parser->Deserialize(Mrm::GetDefaultPmr(), *serialized_sensors, 16, 16);

    for(const auto& sensor : deserialized_sensors) {
        if(!sensor->configuration.HasInterpolation())
            continue;

        const auto& calibration_table = sensor->configuration.calibration_table;
        zassert_true(std::ranges::is_sorted(calibration_table, {}, &CalibrationData::voltage), "%s", sensor->id.c_str());
    }
}

namespace {

// Deserialize()'s error, or nothing when it reads the configuration.
std::string DeserializeRejection(SensorsCborParser& parser, const CborSensorsConfig& sensors_config) {
    try {
        parser.Deserialize(Mrm::GetDefaultPmr(), sensors_config, 16, 16);
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    return {};
}

// sensor_2, whose table has five points.
bool HasFivePointTable(const CborSensorConfig& sensor_config) {
    return sensor_config.configuration.calibration_table.float32float.size() == 5;
}

} // namespace

ZTEST(sensors_parser, test_Deserialize_refuses_a_non_finite_voltage_before_ordering_the_table) {
    SensorsCborParser parser(nullptr);
    const auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    auto& sensor_configs = serialized_sensors->CborSensorConfig_m;
    const auto sensor_config = std::ranges::find_if(sensor_configs, HasFivePointTable);
    zassert_true(sensor_config != sensor_configs.end());

    // Ordering a table with a NaN in it would break the sort's ordering requirement.
    auto& points = sensor_config->configuration.calibration_table.float32float;
    std::ranges::reverse(points);
    points[2].float32float_key = std::numeric_limits<float>::quiet_NaN();

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(error.ends_with("Calibration table must hold finite numbers."), "%s", error.c_str());
}

ZTEST(sensors_parser, test_Deserialize_refuses_a_repeated_voltage) {
    SensorsCborParser parser(nullptr);
    const auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    auto& sensor_configs = serialized_sensors->CborSensorConfig_m;
    const auto sensor_config = std::ranges::find_if(sensor_configs, HasFivePointTable);
    zassert_true(sensor_config != sensor_configs.end());

    // A CBOR map with a repeated key, which the decoder lets through.
    auto& points = sensor_config->configuration.calibration_table.float32float;
    points[3].float32float_key = points[1].float32float_key;

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(error.ends_with("Calibration voltages must be unique."), "%s", error.c_str());
}

ZTEST(sensors_parser, test_Deserialize_reads_the_connection_string_strictly) {
    SensorsCborParser parser(nullptr);

    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "engine_rpm");
    sensor->configuration.type = SensorType::CANBUS_ANALOG;
    sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 1, 100, "rpm");

    auto serialized_sensors = parser.Serialize({ sensor }, 16, 16);
    auto& connection_string = serialized_sensors->CborSensorConfig_m.front().configuration.connection_string;

    // Read as octal, 0100 was frame 64.
    const std::string leading_zero = "1/0100/rpm";
    connection_string = CborHelpers::ToZcborString(leading_zero);

    const auto sensors = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_sensors, 16, 16);
    zassert_equal(sensors.size(), 1);
    zassert_not_null(sensors.front()->configuration.canbus_source);
    zassert_equal(sensors.front()->configuration.canbus_source->frame_id, 100);

    const std::string trailing_text = "1/100abc/rpm";
    connection_string = CborHelpers::ToZcborString(trailing_text);

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(
        error == "Invalid Sensor configuration. Sensor ID: engine_rpm. Connection string frame ID must be a number from 0 to 0x1FFFFFFF.",
        "%s", error.c_str());
}

ZTEST(sensors_parser, test_Deserialize_refuses_a_repeated_sensor_id) {
    SensorsCborParser parser(nullptr);
    const auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    // sensor_4 stored as a second sensor_1, which ordering the sensors would drop.
    auto& sensor_configs = serialized_sensors->CborSensorConfig_m;
    sensor_configs[3].id = sensor_configs[0].id;

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(error == "Invalid Sensor configuration. Sensor ID: sensor_1. Duplicate sensor ID.", "%s", error.c_str());
}

ZTEST(sensors_parser, test_Deserialize_names_the_sensor_whose_expression_is_wrong) {
    SensorsCborParser parser(nullptr);
    const auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);
    auto& sensor_configs = serialized_sensors->CborSensorConfig_m;

    const std::string unknown_reference = "sensor_9 + 1";
    sensor_configs[2].configuration.expression = CborHelpers::ToZcborString(unknown_reference);

    auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(
        error == "Invalid Sensor configuration. Sensor ID: sensor_3. Expression references unknown sensor sensor_9.",
        "%s", error.c_str());

    const std::string unfinished = "x +";
    sensor_configs[2].configuration.expression = sensor_configs[0].configuration.expression;
    sensor_configs[1].configuration.expression = CborHelpers::ToZcborString(unfinished);

    error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(error.starts_with("Invalid Sensor configuration. Sensor ID: sensor_2. Invalid expression"), "%s", error.c_str());
}

namespace {

constexpr uint8_t CBOR_FALSE = 0xF4;
constexpr uint8_t CBOR_TRUE = 0xF5;

// Offset of the byte after @p text, encoded as a text string shorter than 24 bytes.
size_t OffsetAfterTstr(std::span<const uint8_t> bytes, std::string_view text) {
    std::vector<uint8_t> encoded;
    encoded.push_back(static_cast<uint8_t>(0x60 + text.size()));
    encoded.insert(encoded.end(), text.begin(), text.end());

    const auto found = std::search(bytes.begin(), bytes.end(), encoded.begin(), encoded.end());
    zassert_true(found != bytes.end(), "%s is not encoded", std::string(text).c_str());

    return static_cast<size_t>(found - bytes.begin()) + encoded.size();
}

} // namespace

ZTEST(sensors_parser, test_is_enabled_survives_Serialize_and_Deserialize) {
    SensorsCborParser parser(nullptr);
    auto sensors = sensors_parser_GetTestSensors();

    // sensor_3 reads the enabled sensor_1; nothing reads sensor_4.
    sensors[2]->is_enabled = false;
    sensors[3]->is_enabled = false;

    auto serialized_sensors = parser.Serialize(sensors, 16, 16);
    const auto& sensor_configs = serialized_sensors->CborSensorConfig_m;
    zassert_true(sensor_configs[0].is_enabled);
    zassert_true(sensor_configs[1].is_enabled);
    zassert_false(sensor_configs[2].is_enabled);
    zassert_false(sensor_configs[3].is_enabled);

    auto deserialized_sensors = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_sensors, 16, 16);
    sensors_parser_CompareSensors(sensors, deserialized_sensors);
}

ZTEST(sensors_parser, test_is_enabled_survives_a_round_trip_through_cbor_bytes) {
    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto sensors = sensors_parser_GetTestSensors();
    sensors[3]->is_enabled = false;

    const auto bytes = serializer.Serialize(*parser.Serialize(sensors, 16, 16));
    zassert_false(bytes.empty(), "The size calculation must count the flag");

    const auto decoded = serializer.Deserialize(bytes);
    zassert_not_null(decoded.get());

    auto deserialized_sensors = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded, 16, 16);
    sensors_parser_CompareSensors(sensors, deserialized_sensors);
}

ZTEST(sensors_parser, test_is_enabled_follows_the_id_and_cannot_be_left_out) {
    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto sensors = sensors_parser_GetTestSensors();
    sensors[3]->is_enabled = false;

    auto bytes = serializer.Serialize(*parser.Serialize(sensors, 16, 16));
    zassert_false(bytes.empty());

    const size_t sensor_1_flag = OffsetAfterTstr(bytes, "sensor_1");
    zassert_equal(bytes[sensor_1_flag], CBOR_TRUE, "An enabled sensor's id is followed by true");
    zassert_equal(bytes[OffsetAfterTstr(bytes, "sensor_4")], CBOR_FALSE, "A disabled sensor's id is followed by false");

    // The layout from before the flag existed is refused rather than read as enabled.
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(sensor_1_flag));
    zassert_is_null(serializer.Deserialize(bytes).get(), "A sensor without is_enabled must not decode");
}

ZTEST(sensors_parser, test_Deserialize_refuses_an_expression_that_reads_a_disabled_sensor) {
    SensorsCborParser parser(nullptr);
    const auto sensors = sensors_parser_GetTestSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    // sensor_1 reads sensor_2; the stored flag is read before the configuration is validated.
    serialized_sensors->CborSensorConfig_m[1].is_enabled = false;

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(
        error == "Invalid Sensor configuration. Sensor ID: sensor_1. Expression references disabled sensor sensor_2.",
        "%s", error.c_str());
}

namespace {

// A debounced switch with neither of the optional fields before the conditioning, so the decoder
// goes from script_path straight to debounce_ms, and a debounced derived indicator.
std::vector<std::shared_ptr<Sensor>> sensors_parser_GetConditionedSensors() {
    auto sensors = sensors_parser_GetTestSensors();

    auto button = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "button");
    button->configuration.type = SensorType::PHYSICAL_INDICATOR;
    button->configuration.channel = 3;
    button->configuration.debounce_ms = 30;
    button->configuration.input_mode = IndicatorInputMode::Toggle;

    auto moving = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "moving");
    moving->configuration.type = SensorType::VIRTUAL_INDICATOR;
    moving->configuration.expression = "sensor_1 > 50";
    moving->configuration.debounce_ms = 2000;

    sensors.push_back(std::move(button));
    sensors.push_back(std::move(moving));

    return sensors;
}

const CborSensorConfig& FindSensorConfig(const CborSensorsConfig& sensors_config, std::string_view sensor_id) {
    const auto& sensor_configs = sensors_config.CborSensorConfig_m;
    const auto sensor_config = std::ranges::find_if(sensor_configs, [sensor_id](const CborSensorConfig& candidate) {
        return std::string_view(reinterpret_cast<const char*>(candidate.id.value), candidate.id.len) == sensor_id;
    });
    zassert_true(sensor_config != sensor_configs.end(), "%s is not serialized", std::string(sensor_id).c_str());

    return *sensor_config;
}

} // namespace

ZTEST(sensors_parser, test_conditioning_survives_Serialize_and_Deserialize) {
    SensorsCborParser parser(nullptr);
    auto sensors = sensors_parser_GetConditionedSensors();

    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    const auto& button = FindSensorConfig(*serialized_sensors, "button").configuration;
    zassert_equal(button.debounce_ms, 30);
    zassert_equal(button.input_mode, 1);

    const auto& moving = FindSensorConfig(*serialized_sensors, "moving").configuration;
    zassert_equal(moving.debounce_ms, 2000);
    zassert_equal(moving.input_mode, 0);

    const auto& analog = FindSensorConfig(*serialized_sensors, "sensor_1").configuration;
    zassert_equal(analog.debounce_ms, 0);
    zassert_equal(analog.input_mode, 0);

    auto deserialized_sensors = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_sensors, 16, 16);
    sensors_parser_CompareSensors(sensors, deserialized_sensors);
}

ZTEST(sensors_parser, test_conditioning_survives_a_round_trip_through_cbor_bytes) {
    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto sensors = sensors_parser_GetConditionedSensors();

    const auto bytes = serializer.Serialize(*parser.Serialize(sensors, 16, 16));
    zassert_false(bytes.empty(), "The size calculation must count the conditioning");

    const auto decoded = serializer.Deserialize(bytes);
    zassert_not_null(decoded.get());

    auto deserialized_sensors = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded, 16, 16);
    sensors_parser_CompareSensors(sensors, deserialized_sensors);
}

ZTEST(sensors_parser, test_a_sensor_built_in_code_encodes_the_default_conditioning) {
    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto sensors = sensors_parser_GetTestSensors();

    auto serialized_sensors = parser.Serialize(sensors, 16, 16);
    for(const auto& sensor_config : serialized_sensors->CborSensorConfig_m) {
        zassert_equal(sensor_config.configuration.debounce_ms, 0);
        zassert_equal(sensor_config.configuration.input_mode, 0);
    }

    const auto decoded = serializer.Deserialize(serializer.Serialize(*serialized_sensors));
    zassert_not_null(decoded.get(), "The defaults are encoded, not left out");

    for(const auto& sensor : parser.Deserialize(Mrm::GetDefaultPmr(), *decoded, 16, 16)) {
        zassert_equal(sensor->configuration.debounce_ms, 0, "%s", sensor->id.c_str());
        zassert_equal(sensor->configuration.input_mode, IndicatorInputMode::Level, "%s", sensor->id.c_str());
    }
}

ZTEST(sensors_parser, test_conditioning_cannot_be_left_out) {
    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto sensors = sensors_parser_GetConditionedSensors();

    // A debounce that encodes to bytes found nowhere else.
    const auto button = std::ranges::find_if(sensors, [](const auto& sensor) { return sensor->id == "button"; });
    (*button)->configuration.debounce_ms = 0x1234;

    auto bytes = serializer.Serialize(*parser.Serialize(sensors, 16, 16));
    zassert_false(bytes.empty());

    // debounce_ms (uint16 0x1234), input_mode 1, then the end of button's configuration list.
    const std::array<uint8_t, 5> conditioning = { 0x19, 0x12, 0x34, 0x01, 0xFF };
    const auto found = std::ranges::search(bytes, conditioning);
    zassert_false(found.empty(), "button's conditioning is not encoded where expected");

    // The layout from before the fields existed is refused rather than read with the defaults.
    bytes.erase(found.begin(), found.begin() + 4);
    zassert_is_null(serializer.Deserialize(bytes).get(), "A sensor without debounce_ms and input_mode must not decode");
}

ZTEST(sensors_parser, test_Deserialize_refuses_an_unknown_input_mode) {
    SensorsCborParser parser(nullptr);
    // The CBOR strings point into the sensors.
    const auto sensors = sensors_parser_GetConditionedSensors();
    auto serialized_sensors = parser.Serialize(sensors, 16, 16);

    for(auto& sensor_config : serialized_sensors->CborSensorConfig_m) {
        if(std::string_view(reinterpret_cast<const char*>(sensor_config.id.value), sensor_config.id.len) == "button")
            sensor_config.configuration.input_mode = 7;
    }

    const auto error = DeserializeRejection(parser, *serialized_sensors);
    zassert_true(
        error == "Invalid Sensor configuration. Sensor ID: button. Invalid indicator input mode.",
        "%s", error.c_str());
}
