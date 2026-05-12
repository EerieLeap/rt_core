#pragma once

#include "subsys/adc/models/adc_configuration.h"

namespace eerie_leap::domain::sensor_domain::configuration::parsers {

using eerie_leap::subsys::adc::models::AdcConfiguration;

class AdcConfigurationValidator {
private:
    static void ValidateSamples(const AdcConfiguration& configuration);
    static void ValidateChannels(const AdcConfiguration& configuration);

public:
    static void Validate(const AdcConfiguration& configuration);
};

} // namespace eerie_leap::domain::sensor_domain::configuration::parsers
