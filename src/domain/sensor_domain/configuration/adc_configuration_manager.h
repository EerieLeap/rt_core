#pragma once

#include <memory>
#include <memory_resource>
#include <string_view>

#include "utilities/memory/heap_allocator.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/json/configs/json_adc_config.h"
#include "configuration/services/json_configuration_service.h"

#include "subsys/adc/models/adc_configuration.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/adc/adc_factory.hpp"

#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"
#include "domain/configuration_domain/utilities/i_json_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_cbor_parser.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_json_parser.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::configuration::json::configs::JsonAdcConfig;
using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::configuration_domain::utilities::IJsonConfigurationManager;
using eerie_leap::domain::sensor_domain::configuration::parsers::AdcConfigurationCborParser;
using eerie_leap::domain::sensor_domain::configuration::parsers::AdcConfigurationJsonParser;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationManager : public ICborConfigurationManager, public IJsonConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service_;
    std::unique_ptr<config_service::JsonConfigurationService<JsonAdcConfig>> json_configuration_service_;

    std::unique_ptr<AdcConfigurationCborParser> cbor_parser_;
    std::unique_ptr<AdcConfigurationJsonParser> json_parser_;

    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<AdcConfiguration> configuration_;

    uint32_t json_config_checksum_;

    bool ApplyJsonConfiguration(bool fs_load, std::string_view json_str = {});
    bool CreateDefaultConfiguration();

public:
    AdcConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
        std::unique_ptr<config_service::JsonConfigurationService<JsonAdcConfig>> json_configuration_service,
        std::shared_ptr<IAdcManager> adc_manager);

    bool Update(const AdcConfiguration& configuration, bool internal_only = false);
    std::shared_ptr<IAdcManager> Get(bool force_load = false);

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
    std::pmr::vector<uint8_t> GetCborConfiguration() override;

    bool ApplyJsonConfiguration(std::string_view json_str) override;
    std::pmr::string GetJsonConfiguration() override;
};

} // namespace eerie_leap::domain::sensor_domain::configuration
