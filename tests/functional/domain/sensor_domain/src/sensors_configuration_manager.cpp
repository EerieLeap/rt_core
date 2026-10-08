#include <array>
#include <memory>
#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/expression_engine/expression_evaluator.h"

#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::domain::sensor_domain::configuration;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::expression_engine;

ZTEST_SUITE(sensors_configuration_manager, NULL, NULL, NULL, NULL, NULL);

std::vector<std::shared_ptr<Sensor>> sensors_configuration_manager_SetupTestSensors() {
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
        sensor_1, sensor_2, sensor_3, sensor_4 };

    return sensors;
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service);

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service),
        nullptr,
        16,
        16);

    auto sensors = sensors_configuration_manager_SetupTestSensors();
    sensors_configuration_manager->Update(sensors);

    auto saved_sensors = sensors_configuration_manager->Get();

    zassert_equal(saved_sensors->size(), sensors.size());

    for(size_t i = 0; i < sensors.size(); ++i) {
        std::shared_ptr<Sensor> saved_sensor = nullptr;
        for(size_t j = 0; j < saved_sensors->size(); ++j) {
            if(strcmp(saved_sensors->at(j)->id.c_str(), sensors[i]->id.c_str()) == 0) {
                saved_sensor = saved_sensors->at(j);
                break;
            }
        }
        zassert_true(saved_sensor != nullptr);

        zassert_true(saved_sensor->metadata.name == sensors[i]->metadata.name);
        zassert_true(saved_sensor->metadata.unit == sensors[i]->metadata.unit);
        zassert_true(saved_sensor->metadata.description == sensors[i]->metadata.description);

        zassert_true(saved_sensor->configuration.type == sensors[i]->configuration.type);
        if(saved_sensor->configuration.channel.has_value() && sensors[i]->configuration.channel.has_value()) {
            zassert_true(saved_sensor->configuration.channel.value() == sensors[i]->configuration.channel.value());
        } else {
            zassert_true(!saved_sensor->configuration.channel.has_value() && !sensors[i]->configuration.channel.has_value());
        }
        zassert_true(saved_sensor->configuration.sampling_rate_ms == sensors[i]->configuration.sampling_rate_ms);
        zassert_true(saved_sensor->configuration.interpolation_method == sensors[i]->configuration.interpolation_method);
        zassert_true(saved_sensor->configuration.calibration_table.size() == sensors[i]->configuration.calibration_table.size());
        zassert_true(saved_sensor->configuration.expression == sensors[i]->configuration.expression);
    }
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service);

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service),
        nullptr,
        16,
        16);

    auto sensors = sensors_configuration_manager_SetupTestSensors();
    sensors_configuration_manager->Update(sensors);

    cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service);
    sensors_configuration_manager = nullptr;
    sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service),
        nullptr,
        16,
        16);

    auto saved_sensors = sensors_configuration_manager->Get();

    zassert_equal(saved_sensors->size(), sensors.size());

    for(size_t i = 0; i < sensors.size(); ++i) {
        std::shared_ptr<Sensor> saved_sensor = nullptr;
        for(size_t j = 0; j < saved_sensors->size(); ++j) {
            if(strcmp(saved_sensors->at(j)->id.c_str(), sensors[i]->id.c_str()) == 0) {
                saved_sensor = saved_sensors->at(j);
                break;
            }
        }
        zassert_true(saved_sensor != nullptr);

        zassert_true(saved_sensor->metadata.name == sensors[i]->metadata.name);
        zassert_true(saved_sensor->metadata.unit == sensors[i]->metadata.unit);
        zassert_true(saved_sensor->metadata.description == sensors[i]->metadata.description);

        zassert_true(saved_sensor->configuration.type == sensors[i]->configuration.type);
        if(saved_sensor->configuration.channel.has_value() && sensors[i]->configuration.channel.has_value()) {
            zassert_true(saved_sensor->configuration.channel.value() == sensors[i]->configuration.channel.value());
        } else {
            zassert_true(!saved_sensor->configuration.channel.has_value() && !sensors[i]->configuration.channel.has_value());
        }
        zassert_true(saved_sensor->configuration.sampling_rate_ms == sensors[i]->configuration.sampling_rate_ms);
        zassert_true(saved_sensor->configuration.interpolation_method == sensors[i]->configuration.interpolation_method);
        zassert_true(saved_sensor->configuration.calibration_table.size() == sensors[i]->configuration.calibration_table.size());
        zassert_true(saved_sensor->configuration.expression == sensors[i]->configuration.expression);
    }
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_valid_sensor_id) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service);

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service),
        nullptr,
        16,
        16);

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
    sensor_1->configuration.sampling_rate_ms = 100;
    sensor_1->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_1->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    auto sensor_2 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "_sensor_2");

    sensor_2->metadata.name = "Sensor 2";
    sensor_2->metadata.unit = "km/h";
    sensor_2->metadata.description = "Test Sensor 2";

    sensor_2->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_2->configuration.channel = 0;
    sensor_2->configuration.sampling_rate_ms = 100;
    sensor_2->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_2->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "_");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_3->configuration.channel = 0;
    sensor_3->configuration.sampling_rate_ms = 100;
    sensor_3->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_3->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    std::vector<std::shared_ptr<Sensor>> sensors {
        sensor_1, sensor_2, sensor_3 };

    for(auto sensor : sensors)
        sensors_configuration_manager->Update({sensor});
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_invalid_sensor_id) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service);

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service),
        nullptr,
        16,
        16);

    std::pmr::vector<CalibrationData> calibration_data_1 {
        {0.0, 0.0},
        {3.3, 100.0}
    };
    auto calibration_data_1_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_1);

    auto sensor_1 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "1_sensor_1");

    sensor_1->metadata.name = "Sensor 1";
    sensor_1->metadata.unit = "km/h";
    sensor_1->metadata.description = "Test Sensor 1";

    sensor_1->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_1->configuration.channel = 0;
    sensor_1->configuration.sampling_rate_ms = 100;
    sensor_1->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_1->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    auto sensor_2 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "#sensor_2");

    sensor_2->metadata.name = "Sensor 2";
    sensor_2->metadata.unit = "km/h";
    sensor_2->metadata.description = "Test Sensor 2";

    sensor_2->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_2->configuration.channel = 0;
    sensor_2->configuration.sampling_rate_ms = 100;
    sensor_2->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_2->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    auto sensor_3 = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "3");

    sensor_3->metadata.name = "Sensor 3";
    sensor_3->metadata.unit = "km/h";
    sensor_3->metadata.description = "Test Sensor 3";

    sensor_3->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor_3->configuration.channel = 0;
    sensor_3->configuration.sampling_rate_ms = 100;
    sensor_3->configuration.interpolation_method = InterpolationMethod::CUBIC_SPLINE;
    sensor_3->configuration.calibration_table.assign(calibration_data_1_ptr->begin(), calibration_data_1_ptr->end());

    std::vector<std::shared_ptr<Sensor>> sensors {
        sensor_1, sensor_2, sensor_3 };

    for(auto sensor : sensors)
        zassert_false(sensors_configuration_manager->Update({sensor}), "Invalid sensor configuration update expected to fail.");
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_empty_configuration_is_kept_in_memory) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service),
        nullptr,
        16,
        16);

    zassert_true(sensors_configuration_manager->Update({}));
    zassert_true(fs_service->DeleteFile("config/sensors_config.cbor"));

    // No sensors is a configuration too, not a reason to go back to storage.
    const auto sensors = sensors_configuration_manager->Get();
    zassert_true(sensors != nullptr);
    zassert_true(sensors->empty());
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_rejected_stored_configuration_is_kept) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    // A stored configuration that fails to load must not be replaced by the default: the failure
    // may be transient, and overwriting would destroy the user's sensors.
    const std::array<uint8_t, 6> stored = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
    zassert_true(fs_service->CreateDirectory("config"));
    zassert_true(fs_service->WriteFile("config/sensors_config.cbor", stored.data(), stored.size()));

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service),
        nullptr,
        16,
        16);

    const auto sensors = sensors_configuration_manager->Get();
    zassert_true(sensors != nullptr, "A default stands in for the rejected configuration");
    zassert_true(sensors->empty());

    zassert_false(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_equal(sensors_configuration_manager->GetCborConfigurationInfo().size, stored.size());
    zassert_equal(fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), stored.size(), "The stored file is untouched");

    // A valid update replaces it as usual.
    zassert_true(sensors_configuration_manager->Update(sensors_configuration_manager_SetupTestSensors()));
    zassert_true(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_not_equal(fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), stored.size());
}
