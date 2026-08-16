#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

namespace eerie_leap::domain::canbus_domain::utilities {

using eerie_leap::domain::canbus_domain::models::CanMessageConfiguration;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

class CanFrameBuilder {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

public:
    explicit CanFrameBuilder(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    std::vector<uint8_t> Build(const CanMessageConfiguration& message_configuration) const;
};

} // namespace eerie_leap::domain::canbus_domain::utilities
