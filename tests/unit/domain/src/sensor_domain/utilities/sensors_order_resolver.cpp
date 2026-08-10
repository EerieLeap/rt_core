#include <algorithm>
#include <string>
#include <unordered_set>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "subsys/math_parser/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensors_order_resolver.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::subsys::math_parser;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;

ZTEST_SUITE(sensors_order_resolver, NULL, NULL, NULL, NULL, NULL);

std::shared_ptr<Sensor> sensors_order_resolver_MakeSensor(std::string_view id, const char* expression) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);

    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.channel = std::nullopt;
    sensor->configuration.sampling_rate_ms = 1000;

    if(expression != nullptr)
        sensor->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), expression);

    return sensor;
}

int sensors_order_resolver_IndexOf(const std::vector<std::shared_ptr<Sensor>>& sensors, std::string_view id) {
    for(size_t i = 0; i < sensors.size(); ++i)
        if(sensors[i]->id == id)
            return static_cast<int>(i);

    return -1;
}

bool sensors_order_resolver_DependenciesComeFirst(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    for(size_t i = 0; i < sensors.size(); ++i) {
        if(sensors[i]->configuration.expression_evaluator == nullptr)
            continue;

        auto variables = sensors[i]->configuration.expression_evaluator->GetVariableNames();
        variables.erase("x");

        for(const auto& variable : variables) {
            int position = sensors_order_resolver_IndexOf(sensors, variable);
            if(position < 0 || static_cast<size_t>(position) >= i)
                return false;
        }
    }

    return true;
}

std::vector<std::shared_ptr<Sensor>> sensors_order_resolver_GetTestSensors() {
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
    sensor_1->configuration.voltage_interpolator = make_unique_pmr<LinearVoltageInterpolator>(Mrm::GetDefaultPmr(), calibration_data_1_ptr);
    sensor_1->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "x * 2 + sensor_2 + 1");

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
    sensor_2->configuration.voltage_interpolator = make_unique_pmr<LinearVoltageInterpolator>(Mrm::GetDefaultPmr(), calibration_data_2_ptr);
    sensor_2->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "x * 4 + 1.6");

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor_3->configuration.channel = std::nullopt;
    sensor_3->configuration.sampling_rate_ms = 2000;
    sensor_3->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "sensor_1 + 8.34");

    auto sensor_4 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_4");

    sensor_4->metadata.name = "Sensor 4";
    sensor_4->metadata.unit = "km/h";
    sensor_4->metadata.description = "Test Sensor 4";

    sensor_4->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_4->configuration.channel = 4;
    sensor_4->configuration.sampling_rate_ms = 2000;
    sensor_4->configuration.voltage_interpolator = make_unique_pmr<CubicSplineVoltageInterpolator>(Mrm::GetDefaultPmr(), calibration_data_2_ptr);

    auto sensor_5 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_5");

    sensor_5->metadata.name = "Sensor 5";
    sensor_5->metadata.unit = "km/h";
    sensor_5->metadata.description = "Test Sensor 5";

    sensor_5->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_5->configuration.channel = 4;
    sensor_5->configuration.sampling_rate_ms = 2000;
    sensor_5->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "sensor_6 + 2.34");

    auto sensor_6 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_6");
    sensor_6->metadata.name = "Sensor 6";
    sensor_6->metadata.unit = "km/h";
    sensor_6->metadata.description = "Test Sensor 6";

    sensor_6->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_6->configuration.channel = 4;
    sensor_6->configuration.sampling_rate_ms = 2000;
    sensor_6->configuration.expression_evaluator = make_unique_pmr<ExpressionEvaluator>(Mrm::GetDefaultPmr(), "sensor_5 + 4.34");

    std::vector<std::shared_ptr<Sensor>> sensors = {
        sensor_1, sensor_2, sensor_3, sensor_4, sensor_5, sensor_6 };

    return sensors;
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder) {
    auto sensors = sensors_order_resolver_GetTestSensors();

    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();

    auto oredered_sensors = sensors_order_resolver->GetProcessingOrder();
    zassert_equal(oredered_sensors.size(), 0);

    sensors_order_resolver->AddSensor(sensors[1]);
    oredered_sensors = sensors_order_resolver->GetProcessingOrder();
    zassert_equal(oredered_sensors.size(), 1);

    sensors_order_resolver->AddSensor(sensors[0]);
    sensors_order_resolver->AddSensor(sensors[2]);
    oredered_sensors = sensors_order_resolver->GetProcessingOrder();
    zassert_equal(oredered_sensors.size(), 3);
    zassert_str_equal(oredered_sensors[0]->id.c_str(), "sensor_2");
    zassert_str_equal(oredered_sensors[1]->id.c_str(), "sensor_1");
    zassert_str_equal(oredered_sensors[2]->id.c_str(), "sensor_3");

    sensors_order_resolver->AddSensor(sensors[3]);
    oredered_sensors = sensors_order_resolver->GetProcessingOrder();
    zassert_equal(oredered_sensors.size(), 4);
    zassert_str_equal(oredered_sensors[0]->id.c_str(), "sensor_4");
    zassert_str_equal(oredered_sensors[1]->id.c_str(), "sensor_2");
    zassert_str_equal(oredered_sensors[2]->id.c_str(), "sensor_1");
    zassert_str_equal(oredered_sensors[3]->id.c_str(), "sensor_3");
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_missing_dependency) {
    auto sensors = sensors_order_resolver_GetTestSensors();

    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();

    sensors_order_resolver->AddSensor(sensors[0]);
    sensors_order_resolver->AddSensor(sensors[2]);

    bool threw = false;
    try {
        sensors_order_resolver->GetProcessingOrder();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "GetProcessingOrder should reject a missing dependency.");
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_has_cyclic_dependency) {
    auto sensors = sensors_order_resolver_GetTestSensors();

    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();

    sensors_order_resolver->AddSensor(sensors[0]);
    sensors_order_resolver->AddSensor(sensors[1]);
    sensors_order_resolver->AddSensor(sensors[4]);
    sensors_order_resolver->AddSensor(sensors[5]);

    bool threw = false;
    try {
        sensors_order_resolver->GetProcessingOrder();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "GetProcessingOrder should reject a cyclic dependency.");
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_rejects_self_dependency) {
    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_a", "sensor_a + 1"));

    bool threw = false;
    try {
        sensors_order_resolver->GetProcessingOrder();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "A sensor depending on itself should be rejected.");
}

ZTEST(sensors_order_resolver, test_AddSensor_ignores_duplicate_ids) {
    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();

    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_a", nullptr));
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_a", nullptr));

    auto ordered_sensors = sensors_order_resolver->GetProcessingOrder();

    zassert_equal(ordered_sensors.size(), 1);
    zassert_str_equal(ordered_sensors[0]->id.c_str(), "sensor_a");
}

ZTEST(sensors_order_resolver, test_expression_variable_x_is_not_a_dependency) {
    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_a", "x * 2 + 1"));

    auto ordered_sensors = sensors_order_resolver->GetProcessingOrder();

    zassert_equal(ordered_sensors.size(), 1);
    zassert_str_equal(ordered_sensors[0]->id.c_str(), "sensor_a");
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_resolves_diamond_dependencies) {
    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();

    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_d", "sensor_b + sensor_c"));
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_b", "sensor_a * 2"));
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_c", "sensor_a + 1"));
    sensors_order_resolver->AddSensor(sensors_order_resolver_MakeSensor("sensor_a", nullptr));

    auto ordered_sensors = sensors_order_resolver->GetProcessingOrder();

    zassert_equal(ordered_sensors.size(), 4);
    zassert_true(sensors_order_resolver_DependenciesComeFirst(ordered_sensors));
    zassert_equal(sensors_order_resolver_IndexOf(ordered_sensors, "sensor_a"), 0);
    zassert_equal(sensors_order_resolver_IndexOf(ordered_sensors, "sensor_d"), 3);
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_returns_each_sensor_once) {
    auto sensors = sensors_order_resolver_GetTestSensors();

    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();
    for (auto index : { 0, 1, 2, 3 })
        sensors_order_resolver->AddSensor(sensors[index]);

    auto ordered_sensors = sensors_order_resolver->GetProcessingOrder();

    zassert_equal(ordered_sensors.size(), 4);
    zassert_true(sensors_order_resolver_DependenciesComeFirst(ordered_sensors));

    std::unordered_set<std::string> seen;
    for (const auto& sensor : ordered_sensors)
        seen.insert(std::string(sensor->id));

    zassert_equal(seen.size(), 4);
}

ZTEST(sensors_order_resolver, test_GetProcessingOrder_is_repeatable) {
    auto sensors = sensors_order_resolver_GetTestSensors();

    auto sensors_order_resolver = std::make_shared<SensorsOrderResolver>();
    for (auto index : { 0, 1, 2, 3 })
        sensors_order_resolver->AddSensor(sensors[index]);

    auto first = sensors_order_resolver->GetProcessingOrder();
    auto second = sensors_order_resolver->GetProcessingOrder();

    zassert_equal(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i)
        zassert_str_equal(first[i]->id.c_str(), second[i]->id.c_str());
}
