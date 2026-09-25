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

AdcConfigurationManager::AdcConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
    std::shared_ptr<IAdcManager> adc_manager)
        : CborConfigurationManagerBase("ADC", std::move(cbor_configuration_service)),
        adc_manager_(std::move(adc_manager)) {

    LoadOrCreateDefault();
}

std::shared_ptr<IAdcManager> AdcConfigurationManager::Get(bool force_load) {
    return CborConfigurationManagerBase::Get(force_load) != nullptr ? adc_manager_ : nullptr;
}

pmr_unique_ptr<CborAdcConfig> AdcConfigurationManager::Serialize(const AdcConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<AdcConfiguration> AdcConfigurationManager::Deserialize(const CborAdcConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetDefaultPmr(), cbor_config);
}

void AdcConfigurationManager::OnConfigurationChanged(const std::shared_ptr<AdcConfiguration>& configuration) {
    adc_manager_->UpdateConfiguration(configuration);
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
