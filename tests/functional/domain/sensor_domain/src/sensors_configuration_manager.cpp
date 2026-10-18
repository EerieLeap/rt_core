#include <array>
#include <eerie_memory.hpp>
#include <memory>
#include <string_view>
#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/fs/services/i_fs_service.h"

#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::configuration::services;
using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsCborParser;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::sensor_domain::configuration;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::expression_engine;

ZTEST_SUITE(sensors_configuration_manager, NULL, NULL, NULL, NULL, NULL);

std::vector<std::shared_ptr<Sensor>> sensors_configuration_manager_SetupTestSensors() {
    std::pmr::vector<CalibrationData> calibration_data_1{ { 0.0, 0.0 }, { 3.3, 100.0 } };
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

    std::pmr::vector<CalibrationData> calibration_data_2{ { 0.0, 0.0 }, { 1.0, 29.0 }, { 2.0, 111.0 }, { 2.5, 162.0 },
        { 3.3, 200.0 } };
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

    std::vector<std::shared_ptr<Sensor>> sensors = { sensor_1, sensor_2, sensor_3, sensor_4 };

    return sensors;
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>(
        "sensors_config", fs_service
    );

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service), nullptr, 16, 16
    );

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
            zassert_true(
                !saved_sensor->configuration.channel.has_value() && !sensors[i]->configuration.channel.has_value()
            );
        }
        zassert_true(saved_sensor->configuration.sampling_rate_ms == sensors[i]->configuration.sampling_rate_ms);
        zassert_true(
            saved_sensor->configuration.interpolation_method == sensors[i]->configuration.interpolation_method
        );
        zassert_true(
            saved_sensor->configuration.calibration_table.size() == sensors[i]->configuration.calibration_table.size()
        );
        zassert_true(saved_sensor->configuration.expression == sensors[i]->configuration.expression);
    }
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>(
        "sensors_config", fs_service
    );

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service), nullptr, 16, 16
    );

    auto sensors = sensors_configuration_manager_SetupTestSensors();
    // Nothing reads sensor_4.
    sensors[3]->is_enabled = false;
    zassert_true(sensors_configuration_manager->Update(sensors));

    cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>(
        "sensors_config", fs_service
    );
    sensors_configuration_manager = nullptr;
    sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service), nullptr, 16, 16
    );

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

        zassert_equal(saved_sensor->is_enabled, sensors[i]->is_enabled, "%s", sensors[i]->id.c_str());

        zassert_true(saved_sensor->metadata.name == sensors[i]->metadata.name);
        zassert_true(saved_sensor->metadata.unit == sensors[i]->metadata.unit);
        zassert_true(saved_sensor->metadata.description == sensors[i]->metadata.description);

        zassert_true(saved_sensor->configuration.type == sensors[i]->configuration.type);
        if(saved_sensor->configuration.channel.has_value() && sensors[i]->configuration.channel.has_value()) {
            zassert_true(saved_sensor->configuration.channel.value() == sensors[i]->configuration.channel.value());
        } else {
            zassert_true(
                !saved_sensor->configuration.channel.has_value() && !sensors[i]->configuration.channel.has_value()
            );
        }
        zassert_true(saved_sensor->configuration.sampling_rate_ms == sensors[i]->configuration.sampling_rate_ms);
        zassert_true(
            saved_sensor->configuration.interpolation_method == sensors[i]->configuration.interpolation_method
        );
        zassert_true(
            saved_sensor->configuration.calibration_table.size() == sensors[i]->configuration.calibration_table.size()
        );
        zassert_true(saved_sensor->configuration.expression == sensors[i]->configuration.expression);
    }
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_valid_sensor_id) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>(
        "sensors_config", fs_service
    );

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service), nullptr, 16, 16
    );

    std::pmr::vector<CalibrationData> calibration_data_1{ { 0.0, 0.0 }, { 3.3, 100.0 } };
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

    std::vector<std::shared_ptr<Sensor>> sensors{ sensor_1, sensor_2, sensor_3 };

    for(auto sensor : sensors)
        sensors_configuration_manager->Update({ sensor });
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_Save_config_invalid_sensor_id) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_sensors_configuration_service = std::make_unique<CborConfigurationService<CborSensorsConfig>>(
        "sensors_config", fs_service
    );

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_configuration_service), nullptr, 16, 16
    );

    std::pmr::vector<CalibrationData> calibration_data_1{ { 0.0, 0.0 }, { 3.3, 100.0 } };
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

    std::vector<std::shared_ptr<Sensor>> sensors{ sensor_1, sensor_2, sensor_3 };

    for(auto sensor : sensors)
        zassert_false(
            sensors_configuration_manager->Update({ sensor }), "Invalid sensor configuration update expected to fail."
        );
}

ZTEST(sensors_configuration_manager, test_SensorsConfigurationManager_empty_configuration_is_kept_in_memory) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16
    );

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
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16
    );

    const auto sensors = sensors_configuration_manager->Get();
    zassert_true(sensors != nullptr, "A default stands in for the rejected configuration");
    zassert_true(sensors->empty());

    zassert_false(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_equal(sensors_configuration_manager->GetCborConfigurationInfo().size, stored.size());
    zassert_equal(
        fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), stored.size(), "The stored file is untouched"
    );

    // A valid update replaces it as usual.
    zassert_true(sensors_configuration_manager->Update(sensors_configuration_manager_SetupTestSensors()));
    zassert_true(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_not_equal(fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), stored.size());
}

// A sensors configuration stored before debounce_ms and input_mode existed ends where they are
// required, so it does not decode: like a corrupt one, it is kept and the unit runs on the default.
ZTEST(
    sensors_configuration_manager,
    test_SensorsConfigurationManager_stored_configuration_without_conditioning_is_kept
) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto button = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "button");
    button->configuration.type = SensorType::PHYSICAL_INDICATOR;
    button->configuration.channel = 3;
    button->configuration.debounce_ms = 0x1234;
    button->configuration.input_mode = IndicatorInputMode::Toggle;
    const std::vector<std::shared_ptr<Sensor>> sensors{ button };

    SensorsCborParser parser(nullptr);
    CborSerializer<CborSensorsConfig> serializer;
    auto bytes = serializer.Serialize(*parser.Serialize(sensors, 16, 16));
    zassert_false(bytes.empty());

    // debounce_ms (uint16 0x1234) and input_mode 1, before the end of the configuration list.
    const std::array<uint8_t, 5> conditioning = { 0x19, 0x12, 0x34, 0x01, 0xFF };
    const auto found = std::ranges::search(bytes, conditioning);
    zassert_false(found.empty());
    bytes.erase(found.begin(), found.begin() + 4);

    zassert_true(fs_service->CreateDirectory("config"));
    zassert_true(fs_service->WriteFile("config/sensors_config.cbor", bytes.data(), bytes.size()));

    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16
    );

    const auto loaded = sensors_configuration_manager->Get();
    zassert_true(loaded != nullptr, "A default stands in for the old configuration");
    zassert_true(loaded->empty());

    zassert_true(sensors_configuration_manager->IsStoredConfigurationRejected());
    zassert_false(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_equal(
        fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), bytes.size(), "The stored file is untouched"
    );

    // Writing the configuration again replaces it.
    zassert_true(sensors_configuration_manager->Update(sensors));
    zassert_true(sensors_configuration_manager->GetCborConfigurationInfo().is_applied);
    zassert_equal(fs_service->GetFileSize("config/sensors_config.cbor").value_or(0), bytes.size() + 4);

    auto reloaded = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16
    );
    zassert_false(reloaded->IsStoredConfigurationRejected());
    zassert_equal(reloaded->Get()->size(), 1);
    zassert_equal(reloaded->Get()->front()->configuration.debounce_ms, 0x1234);
    zassert_equal(reloaded->Get()->front()->configuration.input_mode, IndicatorInputMode::Toggle);
}

namespace {

std::shared_ptr<Sensor> sensors_configuration_manager_MakeCanSensor(std::string_view signal_name) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "engine_rpm");
    sensor->configuration.type = SensorType::CANBUS_ANALOG;
    sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 0, 0x100, signal_name);

    return sensor;
}

// Bus 0 with message 0x100, which has one signal.
std::shared_ptr<const CanbusConfiguration>
sensors_configuration_manager_MakeCanbusConfiguration(std::string_view signal_name, bool is_message_enabled = true) {

    auto configuration = std::make_shared<CanbusConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.bus_channel = 0;

    auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = 0x100;
    message->is_enabled = is_message_enabled;

    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.size_bits = 16;
    signal.SetName(signal_name);
    message->signal_configurations.emplace_back(std::move(signal));

    channel.message_configurations.emplace_back(std::move(message));
    configuration->channel_configurations.emplace(0, std::move(channel));

    return configuration;
}

} // namespace

ZTEST(sensors_configuration_manager, test_ApplyCborConfiguration_refuses_a_CAN_signal_the_unit_does_not_have) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    // What a client sends, written where nothing checks it against a CAN bus configuration.
    auto client = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config_client", fs_service), nullptr, 16,
        16
    );
    zassert_true(client->Update({ sensors_configuration_manager_MakeCanSensor("rpm") }));
    const auto cbor = client->GetCborConfiguration();

    auto canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("speed");
    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16,
        [&canbus_configuration] {
            return canbus_configuration;
        }
    );

    std::array<char, 192> reason{};
    zassert_false(sensors_configuration_manager->ApplyCborConfiguration(cbor, reason));
    zassert_str_equal(
        reason.data(),
        "Invalid Sensor configuration. Sensor ID: engine_rpm. CAN message 0x100 on bus 0 has no signal rpm."
    );
    zassert_equal(sensors_configuration_manager->Get()->size(), 0, "The refused configuration is not applied");

    canbus_configuration = nullptr;
    zassert_false(sensors_configuration_manager->ApplyCborConfiguration(cbor, reason));
    zassert_str_equal(
        reason.data(), "Invalid Sensor configuration. Sensor ID: engine_rpm. CAN bus 0 is not configured.",
        "Without a CAN bus configuration there are no channels"
    );

    canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("rpm");
    zassert_true(sensors_configuration_manager->ApplyCborConfiguration(cbor, reason));
    zassert_equal(sensors_configuration_manager->Get()->size(), 1);

    // The stored configuration still loads once the CAN bus configuration has lost the signal.
    canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("speed");
    auto reloaded = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16,
        [&canbus_configuration] {
            return canbus_configuration;
        }
    );
    zassert_false(reloaded->IsStoredConfigurationRejected());
    zassert_equal(reloaded->Get()->size(), 1);

    // The unit's own updates are not checked against it either.
    zassert_true(reloaded->Update({ sensors_configuration_manager_MakeCanSensor("rpm") }));
}

ZTEST(sensors_configuration_manager, test_ApplyCborConfiguration_refuses_a_CAN_signal_of_a_disabled_message) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    // What a client sends: the sensor enabled, then the same sensor disabled.
    auto client = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config_client", fs_service), nullptr, 16,
        16
    );
    zassert_true(client->Update({ sensors_configuration_manager_MakeCanSensor("rpm") }));
    const auto enabled_cbor = client->GetCborConfiguration();

    auto disabled_sensor = sensors_configuration_manager_MakeCanSensor("rpm");
    disabled_sensor->is_enabled = false;
    zassert_true(client->Update({ disabled_sensor }));
    const auto disabled_cbor = client->GetCborConfiguration();

    auto canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("rpm", false);
    auto sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16,
        [&canbus_configuration] {
            return canbus_configuration;
        }
    );

    std::array<char, 192> reason{};
    zassert_false(sensors_configuration_manager->ApplyCborConfiguration(enabled_cbor, reason));
    zassert_str_equal(
        reason.data(), "Invalid Sensor configuration. Sensor ID: engine_rpm. CAN message 0x100 on bus 0 is disabled."
    );
    zassert_equal(sensors_configuration_manager->Get()->size(), 0, "The refused configuration is not applied");

    zassert_true(
        sensors_configuration_manager->ApplyCborConfiguration(disabled_cbor, reason),
        "A disabled sensor may stay bound to a disabled message"
    );
    zassert_equal(sensors_configuration_manager->Get()->size(), 1);
    zassert_false(sensors_configuration_manager->Get()->front()->is_enabled);

    canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("rpm");
    zassert_true(sensors_configuration_manager->ApplyCborConfiguration(enabled_cbor, reason));

    // The stored configuration still loads once the message is disabled.
    canbus_configuration = sensors_configuration_manager_MakeCanbusConfiguration("rpm", false);
    auto reloaded = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service), nullptr, 16, 16,
        [&canbus_configuration] {
            return canbus_configuration;
        }
    );
    zassert_false(reloaded->IsStoredConfigurationRejected());
    zassert_equal(reloaded->Get()->size(), 1);
    zassert_true(reloaded->Get()->front()->is_enabled);
}
