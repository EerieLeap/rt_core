#include <algorithm>
#include <array>
#include <memory>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

#include "domain/sensor_domain/configuration/adc_configuration_manager.h"

using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::utilities;
using namespace eerie_leap::subsys::adc::models;
using namespace eerie_leap::domain::sensor_domain::configuration;

ZTEST_SUITE(adc_configuration_manager, NULL, NULL, NULL, NULL, NULL);

AdcConfiguration adc_configuration_manager_GetTestConfiguration() {
    std::pmr::vector<CalibrationData> adc_calibration_data_samples {
        {0.0, 0.0},
        {5.0, 5.0}
    };

    auto adc_calibration_data_samples_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples);
    auto adc_calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr);

    auto adc_channel_configuration = std::make_shared<AdcChannelConfiguration>(adc_calibrator);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(8);
    for(int i = 0; i < 8; i++)
        channel_configurations.push_back(adc_channel_configuration);

    AdcConfiguration adc_configuration;
    adc_configuration.samples = 40;
    adc_configuration.channel_configurations =
        std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(channel_configurations);

    return adc_configuration;
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    AdcFactory adc_factory(nullptr);
    auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    auto adc_configuration_service = std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service);
    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::move(adc_configuration_service), adc_manager);

    auto adc_configuration = adc_configuration_manager_GetTestConfiguration();

    bool result = adc_configuration_manager->Update(adc_configuration);
    zassert_true(result);

    adc_manager = adc_configuration_manager->Get();

    for(int i = 0; i < adc_manager->GetChannelCount(); i++) {
        auto adc_channel_configuration = adc_manager->GetChannelConfiguration(i);

        zassert_true(adc_channel_configuration->calibrator != nullptr);
        zassert_equal(adc_channel_configuration->calibrator->GetInterpolationMethod(), InterpolationMethod::LINEAR);

        auto calibration_table = adc_channel_configuration->calibrator->GetCalibrationTable();
        zassert_equal(calibration_table->size(), 2);
        zassert_equal(calibration_table->at(0).voltage, 0.0);
        zassert_equal(calibration_table->at(0).value, 0.0);
        zassert_equal(calibration_table->at(1).voltage, 5.0);
        zassert_equal(calibration_table->at(1).value, 5.0);
    }
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    AdcFactory adc_factory(nullptr);
    auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    auto adc_configuration_service = std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service);
    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::move(adc_configuration_service), adc_manager);

    auto adc_configuration = adc_configuration_manager_GetTestConfiguration();

    bool result = adc_configuration_manager->Update(adc_configuration);
    zassert_true(result);

    adc_configuration_service = std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service);
    adc_configuration_manager = nullptr;
    adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::move(adc_configuration_service), adc_manager);

    adc_manager = adc_configuration_manager->Get();

    for(int i = 0; i < adc_manager->GetChannelCount(); i++) {
        auto adc_channel_configuration = adc_manager->GetChannelConfiguration(i);

        zassert_true(adc_channel_configuration->calibrator != nullptr);
        zassert_equal(adc_channel_configuration->calibrator->GetInterpolationMethod(), InterpolationMethod::LINEAR);

        auto calibration_table = adc_channel_configuration->calibrator->GetCalibrationTable();
        zassert_equal(calibration_table->size(), 2);
        zassert_equal(calibration_table->at(0).voltage, 0.0);
        zassert_equal(calibration_table->at(0).value, 0.0);
        zassert_equal(calibration_table->at(1).voltage, 5.0);
        zassert_equal(calibration_table->at(1).value, 5.0);
    }
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_ApplyCborConfiguration_accepts_an_unordered_table) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    AdcFactory adc_factory(nullptr);
    auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service), adc_manager);
    zassert_true(adc_configuration_manager->Update(adc_configuration_manager_GetTestConfiguration()));

    // The import is stored as received, so the ordering has to come from parsing it.
    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    auto exported = adc_configuration_manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());

    for(auto& adc_channel_config : cbor_config->CborAdcChannelConfig_m) {
        adc_channel_config.calibration_table.float32float.back().float32float = 4.0F;
        std::ranges::reverse(adc_channel_config.calibration_table.float32float);
    }

    zassert_true(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config)));

    auto expect_imported_configuration = [](const std::shared_ptr<IAdcManager>& imported_adc_manager) {
        zassert_not_null(imported_adc_manager.get());

        for(int i = 0; i < imported_adc_manager->GetChannelCount(); i++) {
            auto calibration_table = imported_adc_manager->GetChannelConfiguration(i)->calibrator->GetCalibrationTable();

            zassert_equal(calibration_table->size(), 2);
            zassert_equal(calibration_table->at(0).voltage, 0.0F);
            zassert_equal(calibration_table->at(1).voltage, 5.0F);
            zassert_equal(calibration_table->at(1).value, 4.0F);
        }
    };

    expect_imported_configuration(adc_configuration_manager->Get());

    auto reloaded_adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service), adc_manager);

    expect_imported_configuration(reloaded_adc_configuration_manager->Get());
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_ApplyCborConfiguration_reports_why_it_rejects) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    AdcFactory adc_factory(nullptr);
    auto adc_manager = adc_factory.Create();
    adc_manager->Initialize();

    auto adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service), adc_manager);

    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    const auto exported = adc_configuration_manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());
    cbor_config->samples = 0;

    std::array<char, 96> reason{};
    zassert_false(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config), reason));
    zassert_str_equal(reason.data(), "Invalid ADC configuration. Samples must be greater than 0.");
}
