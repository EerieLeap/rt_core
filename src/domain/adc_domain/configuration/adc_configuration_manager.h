#pragma once

#include <memory>
#include <span>

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/adc/i_adc_manager.h"

#include "domain/adc_domain/configuration/parsers/adc_configuration_cbor_parser.h"
#include "domain/adc_domain/models/adc_configuration.h"
#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"

namespace eerie_leap::domain::adc_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::adc_domain::configuration::parsers::AdcConfigurationCborParser;
using eerie_leap::domain::adc_domain::models::AdcConfiguration;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::subsys::adc::IAdcManager;

// Stores the user's ADC configuration (samples, calibrators). The ADC manager is only consulted
// for the device-tree channel count; a stored configuration never touches the hardware, readers
// pick it up when they are rebuilt.
class AdcConfigurationManager :
    public CborConfigurationManagerBase<AdcConfiguration, CborAdcConfig>,
    public IConfigurationManager {
private:
    std::shared_ptr<IAdcManager> adc_manager_;
    AdcConfigurationCborParser cbor_parser_;
    ConfigurationUpdatedHandler configuration_updated_handler_;

    eerie_memory::pmr_unique_ptr<CborAdcConfig> Serialize(const AdcConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<AdcConfiguration> Deserialize(const CborAdcConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    AdcConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
        std::shared_ptr<IAdcManager> adc_manager
    );

    // Called after a configuration has been stored through Update() or ApplyCborConfiguration(), not on load.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool Update(const AdcConfiguration& configuration);
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;

    std::shared_ptr<IAdcManager> GetAdcManager() const {
        return adc_manager_;
    }
};

} // namespace eerie_leap::domain::adc_domain::configuration
