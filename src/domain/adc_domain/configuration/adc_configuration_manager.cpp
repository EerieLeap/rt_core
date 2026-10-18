#include <memory>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "adc_configuration_manager.h"
#include "adc_default_configuration.h"

namespace eerie_leap::domain::adc_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::domain::adc_domain::models;

AdcConfigurationManager::AdcConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
    std::shared_ptr<IAdcManager> adc_manager
) :
    CborConfigurationManagerBase("ADC", std::move(cbor_configuration_service)),
    adc_manager_(std::move(adc_manager)),
    cbor_parser_(static_cast<size_t>(adc_manager_->GetChannelCount())) {

    LoadOrCreateDefault();
}

void AdcConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

bool AdcConfigurationManager::Update(const AdcConfiguration& configuration) {
    if(!CborConfigurationManagerBase::Update(configuration))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

bool AdcConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

pmr_unique_ptr<CborAdcConfig> AdcConfigurationManager::Serialize(const AdcConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<AdcConfiguration> AdcConfigurationManager::Deserialize(const CborAdcConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool AdcConfigurationManager::CreateDefaultConfiguration() {
    const auto adc_calibrator = AdcDefaultConfiguration::CreateCalibrator(Mrm::GetExtPmr());

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations;
    channel_configurations.reserve(adc_manager_->GetChannelCount());
    for(int i = 0; i < adc_manager_->GetChannelCount(); ++i)
        channel_configurations.push_back(std::make_shared<AdcChannelConfiguration>(adc_calibrator));

    const auto configuration = std::make_shared<AdcConfiguration>();
    configuration->samples = AdcDefaultConfiguration::SAMPLES;
    configuration->channel_configurations = std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(
        std::move(channel_configurations)
    );

    return Update(*configuration);
}

} // namespace eerie_leap::domain::adc_domain::configuration
