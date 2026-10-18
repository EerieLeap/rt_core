#pragma once

#include <cstddef>
#include <eerie_memory.hpp>
#include <memory>

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "domain/adc_domain/models/adc_configuration.h"

namespace eerie_leap::domain::adc_domain::configuration::parsers {

using eerie_leap::domain::adc_domain::models::AdcConfiguration;

class AdcConfigurationCborParser {
private:
    size_t channel_count_;

public:
    // channel_count is the device tree's, which configurations are validated and padded against.
    explicit AdcConfigurationCborParser(size_t channel_count);

    eerie_memory::pmr_unique_ptr<CborAdcConfig> Serialize(const AdcConfiguration& adc_configuration);
    // A configuration with fewer channels than the device tree gets the default calibration on the rest.
    eerie_memory::pmr_unique_ptr<AdcConfiguration>
    Deserialize(std::pmr::memory_resource* mr, const CborAdcConfig& adc_config);
};

} // namespace eerie_leap::domain::adc_domain::configuration::parsers
