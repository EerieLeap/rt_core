#include <stdexcept>

#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/script_processor.h"

#include "processing_isr_service.h"
#include "processing_scheduler_service.h"
#include "sensors_processing_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_REGISTER(processing_service_logger);

SensorsProcessingService::SensorsProcessingService(
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<IFsService> sd_fs_service)
        : work_queue_thread_(nullptr),
        sensors_configuration_manager_(std::move(sensors_configuration_manager)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        pipeline_builder_(std::move(sd_fs_service), sensor_readings_frame_),
        reading_processors_(std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>()) {

    work_queue_thread_ = std::make_shared<WorkQueueThread>(
        "processing_service",
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY);

    reading_processors_->push_back(std::make_shared<ExpressionProcessor>(sensor_readings_frame_));
    reading_processors_->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value"));

    pipeline_ = std::make_shared<ReadingPipeline>(sensor_readings_frame_, reading_processors_, time_service);

    if(isr_sensor_reader_factory != nullptr || canbus_service != nullptr) {
        isr_service_ = std::make_shared<ProcessingIsrService>(
            time_service,
            std::move(isr_sensor_reader_factory),
            std::move(canbus_service),
            work_queue_thread_,
            pipeline_);
        processing_services_.push_back(isr_service_);
    }

    if(sensor_reader_factory != nullptr) {
        scheduler_service_ = std::make_shared<ProcessingSchedulerService>(
            std::move(sensor_reader_factory),
            work_queue_thread_,
            pipeline_);
        processing_services_.push_back(scheduler_service_);
    }
};

bool SensorsProcessingService::DoInitialize() {
    if(!work_queue_thread_->Initialize()) {
        LOG_ERR("Failed to initialize the processing work queue.");
        return false;
    }

    for(const auto& processing_service : processing_services_) {
        if(!processing_service->Initialize())
            return false;
    }

    return true;
}

bool SensorsProcessingService::DoStart() {
    const auto sensors = sensors_configuration_manager_->Get();
    if(sensors == nullptr) {
        LOG_ERR("No sensors configuration available.");
        return false;
    }

    // Everything a sample needs is allocated here, once per configuration.
    try {
        generation_ = pipeline_builder_.Build(sensors);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to build the sensor pipeline: %s", e.what());
        return false;
    }

    if(isr_service_ != nullptr)
        isr_service_->SetGeneration(generation_);
    if(scheduler_service_ != nullptr)
        scheduler_service_->SetGeneration(generation_);

    for(const auto& processing_service : processing_services_)
        processing_service->Start();

    LOG_INF("Processing Service started with generation %u, %zu sensors.", generation_->id, generation_->runtimes.size());

    return true;
}

bool SensorsProcessingService::DoStop() {
    for(const auto& processing_service : processing_services_)
        processing_service->Stop();

    // Sources are gone, so nothing refers to the generation any more.
    sensor_readings_frame_->ClearReadings();
    generation_ = nullptr;

    LOG_INF("Processing Service stopped.");

    return true;
}

bool SensorsProcessingService::DoPause() {
    for(const auto& processing_service : processing_services_)
        processing_service->Pause();

    LOG_INF("Processing Service paused.");

    return true;
}

bool SensorsProcessingService::DoResume() {
    for(const auto& processing_service : processing_services_)
        processing_service->Resume();

    LOG_INF("Processing Service resumed.");

    return true;
}

bool SensorsProcessingService::RegisterReadingProcessor(std::shared_ptr<IReadingProcessor> processor) {
    // The work queue iterates the list without a lock, so it only changes while nothing runs.
    if(!IsStopped()) {
        LOG_ERR("Reading processors can only be registered while the processing service is stopped.");
        return false;
    }

    reading_processors_->push_back(std::move(processor));

    return true;
}

} // namespace eerie_leap::domain::sensor_domain::services
