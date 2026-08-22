#include <memory>
#include <stdexcept>

#include "collect_isr_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

CollectIsrReadingProcessor::CollectIsrReadingProcessor(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

void CollectIsrReadingProcessor::ProcessReading(const uint32_t sensor_id_hash) {
    auto reading_optioanl = sensor_readings_frame_->TryGetIsrReading(sensor_id_hash);
    if(!reading_optioanl)
        return;
    auto reading = std::move(reading_optioanl.value());

    try {
        if(reading.status != ReadingStatus::RAW)
            throw std::invalid_argument("Reading is in wrong state");

        reading.source = ReadingSource::PROCESSING;

        sensor_readings_frame_->AddOrUpdateReading(reading);
    } catch (const std::exception& e) {
        reading.status = ReadingStatus::ERROR;
        reading.error_message = e.what();
    }
}

} // namespace eerie_leap::domain::sensor_domain::processors
