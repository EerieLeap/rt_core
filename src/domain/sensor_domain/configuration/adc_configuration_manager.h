#pragma once

#include <memory>
#include <memory_resource>
#include <string_view>

#include "utilities/memory/heap_allocator.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/adc/models/adc_configuration.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/adc/adc_factory.hpp"

#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_cbor_parser.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::sensor_domain::configuration::parsers::AdcConfigurationCborParser;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationManager : public ICborConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service_;

    std::unique_ptr<AdcConfigurationCborParser> cbor_parser_;

    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<AdcConfiguration> configuration_;

    bool CreateDefaultConfiguration();

public:
    AdcConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
        std::shared_ptr<IAdcManager> adc_manager);

    bool Update(const AdcConfiguration& configuration);
    std::shared_ptr<IAdcManager> Get(bool force_load = false);

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
    std::pmr::vector<uint8_t> GetCborConfiguration() override;
};

} // namespace eerie_leap::domain::sensor_domain::configuration
