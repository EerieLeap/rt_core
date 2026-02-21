#pragma once

#include <memory>

#include "utilities/memory/heap_allocator.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/json/configs/json_adc_config.h"
#include "configuration/services/json_configuration_service.h"

#include "subsys/adc/models/adc_configuration.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/adc/adc_factory.hpp"

#include "domain/configuration_domain/utilities/i_json_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_cbor_parser.h"
#include "domain/sensor_domain/configuration/parsers/adc_configuration_json_parser.h"

namespace eerie_leap::domain::sensor_domain::configuration {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::models;
using namespace eerie_leap::domain::configuration_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::configuration::parsers;

class AdcConfigurationManager : public IJsonConfigurationManager {
private:
    std::unique_ptr<CborConfigurationService<CborAdcConfig>> cbor_configuration_service_;
    std::unique_ptr<JsonConfigurationService<JsonAdcConfig>> json_configuration_service_;

    std::unique_ptr<AdcConfigurationCborParser> cbor_parser_;
    std::unique_ptr<AdcConfigurationJsonParser> json_parser_;

    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<AdcConfiguration> configuration_;

    uint32_t json_config_checksum_;

    bool ApplyJsonConfiguration(bool fs_load, std::span<const uint8_t> data = {});
    bool CreateDefaultConfiguration();

public:
    AdcConfigurationManager(
        std::unique_ptr<CborConfigurationService<CborAdcConfig>> cbor_configuration_service,
        std::unique_ptr<JsonConfigurationService<JsonAdcConfig>> json_configuration_service,
        std::shared_ptr<IAdcManager> adc_manager);

    bool Update(const AdcConfiguration& configuration, bool internal_only = false);
    std::shared_ptr<IAdcManager> Get(bool force_load = false);

    bool ApplyJsonConfiguration(std::span<const uint8_t> data) override;
};

} // namespace eerie_leap::domain::sensor_domain::configuration
