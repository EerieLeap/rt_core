#pragma once

#include <memory>
#include <zephyr/kernel.h>

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/i_sensor_reader.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::sensor_readers::ISensorReader;
using eerie_leap::domain::sensor_domain::processors::IReadingProcessor;

struct SensorTask {
    k_timeout_t sampling_rate_ms;
    std::shared_ptr<Sensor> sensor;

    std::shared_ptr<SensorReadingsFrame> readings_frame;
    std::unique_ptr<ISensorReader> reader;
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors;
};

} // namespace eerie_leap::domain::sensor_domain::services
