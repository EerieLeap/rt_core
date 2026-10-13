#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/device_tree/dt_adc.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/adc/adc_emulator.h"
#include "subsys/adc/adc_manager.h"

#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/adc_domain/configuration/adc_default_configuration.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::domain::adc_domain::utilities;
using namespace eerie_leap::domain::adc_domain::models;
using namespace eerie_leap::domain::adc_domain::configuration;

namespace {

// The overlay declares two emulated ADCs: four channels, then two.
constexpr int kFirstAdcChannels = 4;
constexpr int kDtAdcCount = 2;
constexpr int kDtChannelCount = 6;

std::shared_ptr<FsService> MakeFormattedFsService() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    return fs_service;
}

// The first adc_count device-tree ADCs; QEMU targets declare a third one that never initializes.
std::shared_ptr<IAdcManager> MakeAdcManager(int adc_count = kDtAdcCount) {
    DtAdc::Initialize();

    std::vector<std::shared_ptr<IAdc>> adcs;
    for(int i = 0; i < adc_count; i++)
        adcs.push_back(std::make_shared<AdcEmulator>(DtAdc::Get().value().at(i)));

    auto adc_manager = std::make_shared<AdcManager>(std::move(adcs));
    zassert_true(adc_manager->Initialize());

    return adc_manager;
}

std::shared_ptr<AdcConfigurationManager> MakeAdcConfigurationManager(
    std::shared_ptr<FsService> fs_service, std::shared_ptr<IAdcManager> adc_manager) {

    return std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", std::move(fs_service)),
        std::move(adc_manager));
}

std::string ExpectedSamplesReason(uint32_t samples) {
    return "Invalid ADC configuration. Samples must be between 1 and "
        + std::to_string(CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX)
        + ", got "
        + std::to_string(samples)
        + ".";
}

void ExpectSameCalibrator(const AdcCalibrator& expected, const AdcCalibrator& actual, size_t channel) {
    zassert_equal(actual.GetInterpolationMethod(), expected.GetInterpolationMethod(), "channel %zu", channel);

    auto expected_table = expected.GetCalibrationTable();
    auto actual_table = actual.GetCalibrationTable();
    zassert_equal(actual_table->size(), expected_table->size(), "channel %zu", channel);

    for(size_t j = 0; j < expected_table->size(); j++) {
        zassert_equal(actual_table->at(j).voltage, expected_table->at(j).voltage, "channel %zu point %zu", channel, j);
        zassert_equal(actual_table->at(j).value, expected_table->at(j).value, "channel %zu point %zu", channel, j);
    }
}

} // namespace

ZTEST_SUITE(adc_configuration_manager, NULL, NULL, NULL, NULL, NULL);

AdcConfiguration adc_configuration_manager_GetTestConfiguration(int channel_count = kDtChannelCount) {
    std::pmr::vector<CalibrationData> adc_calibration_data_samples {
        {0.0, 0.0},
        {5.0, 5.0}
    };

    auto adc_calibration_data_samples_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples);
    auto adc_calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr);

    auto adc_channel_configuration = std::make_shared<AdcChannelConfiguration>(adc_calibrator);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(channel_count);
    for(int i = 0; i < channel_count; i++)
        channel_configurations.push_back(adc_channel_configuration);

    AdcConfiguration adc_configuration;
    adc_configuration.samples = 40;
    adc_configuration.channel_configurations =
        std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(channel_configurations);

    return adc_configuration;
}

void adc_configuration_manager_ExpectTestConfiguration(const std::shared_ptr<AdcConfiguration>& configuration, int channel_count) {
    zassert_not_null(configuration.get());
    zassert_equal(configuration->channel_configurations->size(), static_cast<size_t>(channel_count));

    for(int i = 0; i < channel_count; i++) {
        auto adc_channel_configuration = configuration->channel_configurations->at(i);

        zassert_true(adc_channel_configuration->calibrator != nullptr);
        zassert_equal(adc_channel_configuration->calibrator->GetInterpolationMethod(), InterpolationMethod::LINEAR);

        auto calibration_table = adc_channel_configuration->calibrator->GetCalibrationTable();
        zassert_equal(calibration_table->size(), 2);
        zassert_equal(calibration_table->at(0).voltage, 0.0F);
        zassert_equal(calibration_table->at(0).value, 0.0F);
        zassert_equal(calibration_table->at(1).voltage, 5.0F);
        zassert_equal(calibration_table->at(1).value, 5.0F);
    }
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_Save_config_successfully_saved) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();
    zassert_equal(adc_manager->GetChannelCount(), kDtChannelCount);

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    bool result = adc_configuration_manager->Update(adc_configuration_manager_GetTestConfiguration());
    zassert_true(result);

    auto saved_configuration = adc_configuration_manager->Get();
    zassert_equal(saved_configuration->samples, 40);
    adc_configuration_manager_ExpectTestConfiguration(saved_configuration, adc_manager->GetChannelCount());
    zassert_equal(adc_configuration_manager->GetAdcManager().get(), adc_manager.get());
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_Save_config_and_Load) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    bool result = adc_configuration_manager->Update(adc_configuration_manager_GetTestConfiguration());
    zassert_true(result);

    adc_configuration_manager = nullptr;
    adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    auto saved_configuration = adc_configuration_manager->Get();
    zassert_equal(saved_configuration->samples, 40);
    adc_configuration_manager_ExpectTestConfiguration(saved_configuration, adc_manager->GetChannelCount());

    // A loaded configuration lives in external RAM, calibration tables included.
    auto calibration_table = saved_configuration->channel_configurations->at(0)->calibrator->GetCalibrationTable();
    zassert_equal(calibration_table->get_allocator().resource(), Mrm::GetExtPmr());
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_ApplyCborConfiguration_accepts_an_unordered_table) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
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

    auto expect_imported_configuration = [&](const std::shared_ptr<AdcConfiguration>& imported_configuration) {
        zassert_not_null(imported_configuration.get());
        zassert_equal(imported_configuration->channel_configurations->size(), static_cast<size_t>(adc_manager->GetChannelCount()));

        for(int i = 0; i < adc_manager->GetChannelCount(); i++) {
            auto calibration_table = imported_configuration->channel_configurations->at(i)->calibrator->GetCalibrationTable();

            zassert_equal(calibration_table->size(), 2);
            zassert_equal(calibration_table->at(0).voltage, 0.0F);
            zassert_equal(calibration_table->at(1).voltage, 5.0F);
            zassert_equal(calibration_table->at(1).value, 4.0F);
        }
    };

    expect_imported_configuration(adc_configuration_manager->Get());

    auto reloaded_adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    expect_imported_configuration(reloaded_adc_configuration_manager->Get());
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_ApplyCborConfiguration_reports_why_it_rejects) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    const auto exported = adc_configuration_manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());
    cbor_config->samples = 0;

    std::array<char, 96> reason{};
    zassert_false(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config), reason));
    zassert_str_equal(reason.data(), ExpectedSamplesReason(0).c_str());
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_ApplyCborConfiguration_rejects_more_channels_than_the_device_tree) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    zassert_true(adc_configuration_manager->Update(adc_configuration_manager_GetTestConfiguration()));

    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    auto cbor_config = serializer.Deserialize(adc_configuration_manager->GetCborConfiguration());
    zassert_not_null(cbor_config.get());

    CborAdcChannelConfig extra_channel_config(std::allocator_arg, Mrm::GetDefaultPmr());
    extra_channel_config = cbor_config->CborAdcChannelConfig_m.back();
    cbor_config->CborAdcChannelConfig_m.push_back(std::move(extra_channel_config));
    zassert_equal(cbor_config->CborAdcChannelConfig_m.size(), kDtChannelCount + 1);

    std::array<char, 96> reason{};
    zassert_false(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config), reason));
    zassert_str_equal(reason.data(), "Invalid ADC configuration. 7 channels configured, the device tree has 6.");

    // The rejected import neither replaced the configuration nor reached the file.
    adc_configuration_manager_ExpectTestConfiguration(adc_configuration_manager->Get(), kDtChannelCount);
    adc_configuration_manager_ExpectTestConfiguration(adc_configuration_manager->Get(true), kDtChannelCount);
}

ZTEST(adc_configuration_manager, test_AdcConfigurationManager_notifies_after_an_update_not_on_load) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);

    int notified = 0;
    adc_configuration_manager->RegisterConfigurationUpdatedHandler([&notified] { notified++; });

    auto adc_configuration = adc_configuration_manager_GetTestConfiguration();
    adc_configuration.samples = 10;
    zassert_true(adc_configuration_manager->Update(adc_configuration));
    zassert_equal(notified, 1, "Update() notifies");

    eerie_leap::configuration::cbor::CborSerializer<CborAdcConfig> serializer;
    auto cbor_config = serializer.Deserialize(adc_configuration_manager->GetCborConfiguration());
    zassert_not_null(cbor_config.get());

    cbor_config->samples = 20;
    zassert_true(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config)));
    zassert_equal(notified, 2, "ApplyCborConfiguration() notifies once");
    zassert_equal(adc_configuration_manager->Get()->samples, 20);

    cbor_config->samples = 0;
    zassert_false(adc_configuration_manager->ApplyCborConfiguration(serializer.Serialize(*cbor_config)));
    zassert_equal(notified, 2, "a rejected configuration does not notify");

    adc_configuration.samples = 0;
    zassert_false(adc_configuration_manager->Update(adc_configuration));
    zassert_equal(notified, 2, "a rejected update does not notify");

    zassert_not_null(adc_configuration_manager->Get(true).get());
    zassert_equal(notified, 2, "loading does not notify");

    adc_configuration_manager->RegisterConfigurationUpdatedHandler(nullptr);
}

ZTEST(adc_configuration_manager, test_LoadOrCreateDefault_pads_a_stored_configuration_with_fewer_channels) {
    auto fs_service = MakeFormattedFsService();

    // Stored by firmware that knew only the first ADC.
    auto first_adc_manager = MakeAdcManager(1);
    zassert_equal(first_adc_manager->GetChannelCount(), kFirstAdcChannels);

    auto stored_configuration = adc_configuration_manager_GetTestConfiguration(kFirstAdcChannels);
    stored_configuration.samples = 25;
    {
        auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, first_adc_manager);
        zassert_true(adc_configuration_manager->Update(stored_configuration));
    }

    // Loaded by firmware whose device tree adds the second one.
    auto adc_manager = MakeAdcManager();
    zassert_equal(adc_manager->GetChannelCount(), kDtChannelCount);

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    auto loaded_configuration = adc_configuration_manager->Get();

    zassert_not_null(loaded_configuration.get());
    zassert_equal(loaded_configuration->samples, 25);
    zassert_equal(loaded_configuration->channel_configurations->size(), kDtChannelCount);

    for(size_t i = 0; i < kFirstAdcChannels; i++) {
        ExpectSameCalibrator(
            *stored_configuration.channel_configurations->at(i)->calibrator,
            *loaded_configuration->channel_configurations->at(i)->calibrator,
            i);
    }

    auto default_calibrator = AdcDefaultConfiguration::CreateCalibrator(Mrm::GetDefaultPmr());
    for(size_t i = kFirstAdcChannels; i < kDtChannelCount; i++)
        ExpectSameCalibrator(*default_calibrator, *loaded_configuration->channel_configurations->at(i)->calibrator, i);

    // The user's calibration was loaded, not replaced by a default.
    zassert_false(adc_configuration_manager->IsStoredConfigurationRejected());
    zassert_true(adc_configuration_manager->GetCborConfigurationInfo().is_applied);
}

ZTEST(adc_configuration_manager, test_Default_configuration_matches_the_device_tree) {
    auto fs_service = MakeFormattedFsService();
    auto adc_manager = MakeAdcManager();

    auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    auto default_configuration = adc_configuration_manager->Get();

    zassert_not_null(default_configuration.get());
    zassert_equal(default_configuration->samples, AdcDefaultConfiguration::SAMPLES);
    zassert_equal(default_configuration->channel_configurations->size(), static_cast<size_t>(adc_manager->GetChannelCount()));
    zassert_equal(default_configuration->channel_configurations->size(), kDtChannelCount);

    auto default_calibrator = AdcDefaultConfiguration::CreateCalibrator(Mrm::GetDefaultPmr());
    for(size_t i = 0; i < kDtChannelCount; i++) {
        auto channel_configuration = default_configuration->channel_configurations->at(i);

        zassert_not_null(channel_configuration.get());
        zassert_not_null(channel_configuration->calibrator.get());
        ExpectSameCalibrator(*default_calibrator, *channel_configuration->calibrator, i);
    }

    // The default was stored, so a reload finds the same configuration rather than creating it again.
    auto reloaded_configuration = MakeAdcConfigurationManager(fs_service, adc_manager)->Get();
    zassert_equal(reloaded_configuration->samples, AdcDefaultConfiguration::SAMPLES);
    zassert_equal(reloaded_configuration->channel_configurations->size(), kDtChannelCount);
}
