#pragma once

#include <eerie_memory.hpp>

#include "configuration/json/configs/json_adc_config.h"
#include "subsys/adc/models/adc_configuration.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using eerie_leap::configuration::json::configs::JsonAdcConfig;
using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationJsonParser {
public:
    AdcConfigurationJsonParser() = default;

    eerie_memory::pmr_unique_ptr<JsonAdcConfig> Serialize(const AdcConfiguration& adc_configuration);
    eerie_memory::pmr_unique_ptr<AdcConfiguration> Deserialize(std::pmr::memory_resource* mr, const JsonAdcConfig& json);
};

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
