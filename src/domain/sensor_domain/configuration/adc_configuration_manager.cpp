#include <memory>
#include <vector>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "adc_configuration_manager.h"

namespace eerie_leap::domain::sensor_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::utilities;

LOG_MODULE_REGISTER(adc_config_ctrl_logger);

AdcConfigurationManager::AdcConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
    std::shared_ptr<IAdcManager> adc_manager)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        adc_manager_(adc_manager),
        configuration_(nullptr) {

    cbor_parser_ = std::make_unique<AdcConfigurationCborParser>();

    std::shared_ptr<IAdcManager> adc_manager_instance = nullptr;

    try {
        adc_manager_instance = Get(true);
    } catch(...) {
        LOG_ERR("Failed to load ADC configuration.");
    }

    if(adc_manager_instance == nullptr) {
        LOG_ERR("Failed to load ADC configuration.");

        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default ADC configuration.");
            return;
        }

        LOG_INF("Default ADC configuration loaded successfully.");
    } else {
        LOG_INF("ADC Configuration Manager initialized successfully.");
    }
}

bool AdcConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
    if(cbor_config == nullptr)
        return false;

    try {
        auto configuration = cbor_parser_->Deserialize(Mrm::GetDefaultPmr(), *cbor_config);

        if(!Update(*configuration))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize CBOR configuration. %s", e.what());
        return false;
    }

    LOG_INF("CBOR configuration loaded successfully.");

    return true;
}

std::pmr::vector<uint8_t> AdcConfigurationManager::GetCborConfiguration() {
    auto cbor_config = cbor_parser_->Serialize(*configuration_);

    return cbor_configuration_service_->Serialize(*cbor_config);
}

bool AdcConfigurationManager::Update(const AdcConfiguration& configuration) {
    try {
        auto cbor_config = cbor_parser_->Serialize(configuration);

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update ADC configuration. %s", e.what());
        return false;
    }

    return Get(true) != nullptr;
}

std::shared_ptr<IAdcManager> AdcConfigurationManager::Get(bool force_load) {
    if (configuration_ != nullptr && !force_load) {
        return adc_manager_;
    }

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    auto configuration = cbor_parser_->Deserialize(Mrm::GetDefaultPmr(), *cbor_config);
    configuration_ = make_shared_pmr<AdcConfiguration>(Mrm::GetDefaultPmr(), std::move(*configuration));
    adc_manager_->UpdateConfiguration(configuration_);

    return adc_manager_;
}

// TODO: Refine default configuration
bool AdcConfigurationManager::CreateDefaultConfiguration() {
    std::pmr::vector<CalibrationData> adc_calibration_data_samples {
        {0.501f, 0.469f},
        {1.0f, 0.968f},
        {2.0f, 1.970f},
        {3.002f, 2.98f},
        {4.003f, 4.01f},
        {5.0f, 5.0f}
    };

    auto adc_calibration_data_samples_ptr = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples);
    auto adc_calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(adc_manager_->GetChannelCount());
    for(int i = 0; i < adc_manager_->GetChannelCount(); ++i)
        channel_configurations.push_back(std::make_shared<AdcChannelConfiguration>(adc_calibrator));

    auto configuration = std::make_shared<AdcConfiguration>();
    configuration->samples = 40;
    configuration->channel_configurations =
        std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(channel_configurations);

    return Update(*configuration);
}

} // namespace eerie_leap::domain::sensor_domain::configuration
