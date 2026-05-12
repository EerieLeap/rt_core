#include <memory>
#include <stdexcept>

#include "adc_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

AdcReadingProcessor::AdcReadingProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
    : sensor_readings_frame_(std::move(sensor_readings_frame)) {}

void AdcReadingProcessor::ProcessReading(const size_t sensor_id_hash) {
    auto reading_optioanl = sensor_readings_frame_->TryGetReading(sensor_id_hash);
    if(!reading_optioanl)
        return;
    auto reading = std::move(reading_optioanl.value());

    try {
        if(reading.sensor->configuration.type != SensorType::PHYSICAL_ANALOG)
            return;

        if(reading.status != ReadingStatus::RAW)
            throw std::invalid_argument("Reading is in wrong state");

        float voltage = reading.value.value();
        reading.value = reading.sensor->configuration.voltage_interpolator->Interpolate(voltage, true);

        reading.metadata.AddTag<float>(ReadingMetadataTag::RAW_VALUE, reading.value.value());
        reading.status = ReadingStatus::INTERPOLATED;

        sensor_readings_frame_->AddOrUpdateReading(reading);
    } catch (const std::exception& e) {
        reading.status = ReadingStatus::ERROR;
        reading.error_message = e.what();
    }
}

} // namespace eerie_leap::domain::sensor_domain::processors
