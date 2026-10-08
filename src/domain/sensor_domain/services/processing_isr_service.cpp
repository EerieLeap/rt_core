#include "domain/sensor_domain/processors/reading_pipeline.hpp"

#include "processing_isr_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::processors;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingIsrService::ProcessingIsrService(
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors)
        : sensor_readings_frame_(std::move(sensor_readings_frame)),
        isr_sensor_reader_factory_(std::move(isr_sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        reading_processors_(std::move(reading_processors)) {};

void ProcessingIsrService::SetGeneration(std::shared_ptr<const SensorGeneration> generation) {
    generation_ = std::move(generation);
}

// Runs on the processing work queue with the reading the reader just produced.
void ProcessingIsrService::ProcessSensor(const SensorRuntime& runtime, SensorReading& reading) {
    ReadingPipeline::Run(*reading_processors_, runtime, reading);

    sensor_readings_frame_->AddOrUpdateReading(reading);

    if(reading.HasError())
        LOG_DBG("Sensor %s: %s", runtime.GetSensor().id.c_str(), ToString(reading.error).data());
}

bool ProcessingIsrService::DoStart() {
    if(generation_ == nullptr)
        return false;

    readers_.clear();
    for(const auto& runtime : generation_->runtimes) {
        if(runtime.update_method != SensorReadingUpdateMethod::ISR)
            continue;

        auto reader = isr_sensor_reader_factory_->Create(
            runtime,
            work_queue_thread_,
            [this](const SensorRuntime& runtime, SensorReading& reading) { ProcessSensor(runtime, reading); });

        if(reader == nullptr)
            continue;

        readers_.push_back(std::move(reader));

        LOG_INF("Created ISR reader for sensor: %s", runtime.GetSensor().id.c_str());
    }

    return true;
}

// Readers go before the generation they refer to.
bool ProcessingIsrService::DoStop() {
    readers_.clear();
    generation_ = nullptr;

    return true;
}

bool ProcessingIsrService::DoPause() {
    readers_.clear();

    return true;
}

bool ProcessingIsrService::DoResume() {
    return DoStart();
}

} // namespace eerie_leap::domain::sensor_domain::services
