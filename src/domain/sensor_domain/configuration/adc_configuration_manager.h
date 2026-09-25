#pragma once

#include <memory>
#include <memory_resource>

#include "utilities/memory/heap_allocator.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/adc/models/adc_configuration.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/adc/adc_factory.hpp"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_cbor_parser.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::sensor_domain::configuration::parsers::AdcConfigurationCborParser;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationManager : public CborConfigurationManagerBase<AdcConfiguration, CborAdcConfig> {
private:
    AdcConfigurationCborParser cbor_parser_;

    std::shared_ptr<IAdcManager> adc_manager_;

    eerie_memory::pmr_unique_ptr<CborAdcConfig> Serialize(const AdcConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<AdcConfiguration> Deserialize(const CborAdcConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;
    void OnConfigurationChanged(const std::shared_ptr<AdcConfiguration>& configuration) override;

public:
    AdcConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
        std::shared_ptr<IAdcManager> adc_manager);

    std::shared_ptr<IAdcManager> Get(bool force_load = false);
};

} // namespace eerie_leap::domain::sensor_domain::configuration
