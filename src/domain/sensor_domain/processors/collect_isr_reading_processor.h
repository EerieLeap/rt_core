#pragma once

#include <memory>

#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

class CollectIsrReadingProcessor : public IReadingProcessor {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

public:
    explicit CollectIsrReadingProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    void ProcessReading(const uint32_t sensor_id_hash) override;
};

} // namespace eerie_leap::domain::sensor_domain::processors
