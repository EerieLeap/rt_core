#pragma once

#include <memory>
#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "subsys/adc/models/adc_configuration.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationCborParser {
public:
    AdcConfigurationCborParser() = default;

    eerie_memory::pmr_unique_ptr<CborAdcConfig> Serialize(const AdcConfiguration& adc_configuration);
    eerie_memory::pmr_unique_ptr<AdcConfiguration> Deserialize(std::pmr::memory_resource* mr, const CborAdcConfig& adc_config);
};

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
