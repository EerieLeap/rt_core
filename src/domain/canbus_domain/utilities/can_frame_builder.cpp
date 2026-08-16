#include <memory>

#include "can_message_codec.h"

#include "can_frame_builder.h"

namespace eerie_leap::domain::canbus_domain::utilities {

CanFrameBuilder::CanFrameBuilder(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
    : sensor_readings_frame_(std::move(sensor_readings_frame)) {}

std::vector<uint8_t> CanFrameBuilder::Build(const CanMessageConfiguration& message_configuration) const {
    return CanMessageCodec::Encode(
        message_configuration,
        [&sensor_readings_frame = sensor_readings_frame_](const CanSignalConfiguration& signal_configuration) {
            return sensor_readings_frame->TryGetReadingValue(signal_configuration.name_hash).value_or(0.0F);
        });
}

} // namespace eerie_leap::domain::canbus_domain::utilities
