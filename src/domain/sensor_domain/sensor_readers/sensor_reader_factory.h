#pragma once

#include <memory>

#include "utilities/guid/guid_generator.h"
#include "subsys/time/i_time_service.h"
#include "subsys/gpio/i_gpio.h"

#include "domain/sensor_domain/configuration/adc_configuration_manager.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/models/sensor.h"
#include "i_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::utilities::guid::GuidGenerator;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::domain::sensor_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::models::Sensor;

class SensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<GuidGenerator> guid_generator_;
    std::shared_ptr<IGpio> gpio_;
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

public:
    SensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<IGpio> gpio,
        std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    virtual ~SensorReaderFactory() = default;

    std::unique_ptr<ISensorReader> Create(std::shared_ptr<Sensor> sensor);
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
