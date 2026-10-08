#pragma once

#include <functional>
#include <memory>

#include "domain/sensor_domain/configuration/adc_configuration_manager.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/adc/models/adc_configuration.h"
#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::adc::AdcChannelConfiguration;
using eerie_leap::domain::sensor_domain::configuration::AdcConfigurationManager;

class SensorReaderPhysicalAnalog : public SensorReaderBase {
private:
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager_;

    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<AdcChannelConfiguration> adc_channel_configuration_;

protected:
    std::function<float ()> AdcChannelReader;

public:
    SensorReaderPhysicalAnalog(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        std::shared_ptr<AdcConfigurationManager> adc_configuration_manager);

    ~SensorReaderPhysicalAnalog() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
