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

    // Moves into the processing stage either way, so a failed reading replaces the previous
    // state instead of staying queued as an ISR reading.
    reading.source = ReadingSource::PROCESSING;

    try {
        if(reading.status != ReadingStatus::RAW)
            throw std::invalid_argument("Reading is in wrong state");
    } catch (const std::exception& e) {
        reading.status = ReadingStatus::ERROR;
        reading.error_message = e.what();
    }

    sensor_readings_frame_->AddOrUpdateReading(reading);
}

} // namespace eerie_leap::domain::sensor_domain::processors
