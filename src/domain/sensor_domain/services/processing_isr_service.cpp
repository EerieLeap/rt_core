#include "domain/sensor_domain/processors/reading_pipeline.hpp"

#include "processing_isr_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::processors;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingIsrService::ProcessingIsrService(
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors)
        : sensors_configuration_manager_(std::move(sensors_configuration_manager)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        isr_sensor_reader_factory_(std::move(isr_sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        reading_processors_(std::move(reading_processors)) {};

// Runs on the processing work queue with the reading the reader just produced.
void ProcessingIsrService::ProcessSensor(const Sensor& sensor, SensorReading& reading) {
    ReadingPipeline::Run(*reading_processors_, sensor, reading);

    sensor_readings_frame_->AddOrUpdateReading(reading);

    if(reading.HasError())
        LOG_DBG("Sensor %s: %s", sensor.id.c_str(), ToString(reading.error).data());
}

bool ProcessingIsrService::DoStart() {
    const auto sensors = sensors_configuration_manager_->Get();
    if(sensors == nullptr)
        return false;

    readers_.clear();
    for(const auto& sensor : *sensors) {
        if(sensor->configuration.GetReadingUpdateMethod() != SensorReadingUpdateMethod::ISR)
            continue;

        auto reader = isr_sensor_reader_factory_->Create(
            sensor,
            work_queue_thread_,
            [this](const Sensor& sensor, SensorReading& reading) { ProcessSensor(sensor, reading); });

        if(reader == nullptr)
            continue;

        readers_.push_back(std::move(reader));

        LOG_INF("Created ISR reader for sensor: %s", sensor->id.c_str());
    }

    return true;
}

bool ProcessingIsrService::DoStop() {
    readers_.clear();

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
