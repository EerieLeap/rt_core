#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "subsys/expression_engine/expression_evaluator.h"

#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/models/sensor.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::subsys::expression_engine;

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
