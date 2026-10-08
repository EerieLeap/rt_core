#pragma once

#include <memory>

#include "subsys/time/i_time_service.h"
#include "subsys/gpio/i_gpio.h"

#include "domain/sensor_domain/configuration/adc_configuration_manager.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "i_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::domain::sensor_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

// Creates the polled reader for a sensor's source kind; null when the unit lacks that source.
class SensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<IGpio> gpio_;
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager_;

public:
    SensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<IGpio> gpio,
        std::shared_ptr<AdcConfigurationManager> adc_configuration_manager);

    virtual ~SensorReaderFactory() = default;

    std::unique_ptr<ISensorReader> Create(const SensorRuntime& runtime);
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
