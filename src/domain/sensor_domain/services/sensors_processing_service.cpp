#include <span>

#include "subsys/time/time_helpers.hpp"
#include "subsys/lua_script/lua_script.h"
#include "domain/sensor_domain/processors/collect_isr_reading_processor.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/script_processor.h"
#include "domain/script_domain/utilities/global_fuctions_registry.h"

#include "processing_isr_service.h"
#include "processing_scheduler_service.h"
#include "sensors_processing_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::lua_script;
using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::script_domain::utilities;

LOG_MODULE_DECLARE(processing_service_logger);

SensorsProcessingService::SensorsProcessingService(
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory)
        : work_queue_thread_(nullptr),
        sensors_configuration_manager_(std::move(sensors_configuration_manager)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        isr_sensor_reader_factory_(std::move(isr_sensor_reader_factory)),
        sensor_reader_factory_(std::move(sensor_reader_factory)),
        reading_processors_(std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>()) {

    work_queue_thread_ = std::make_shared<WorkQueueThread>(
        "processing_service",
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY);

    reading_processors_->push_back(std::make_shared<ExpressionProcessor>(sensor_readings_frame_));
    reading_processors_->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value", sensor_readings_frame_));

    if(isr_sensor_reader_factory_) {
        processing_services_.emplace_back(std::make_unique<ProcessingIsrService>(
            sensors_configuration_manager_,
            sensor_readings_frame_,
            isr_sensor_reader_factory_,
            work_queue_thread_,
            reading_processors_));
    }

    if(sensor_reader_factory_) {
        processing_services_.emplace_back(std::make_unique<ProcessingSchedulerService>(
            sensors_configuration_manager_,
            sensor_readings_frame_,
            sensor_reader_factory_,
            work_queue_thread_,
            reading_processors_));
    }
};

void SensorsProcessingService::Initialize() {
    work_queue_thread_->Initialize();

    for(auto& processing_service : processing_services_)
        processing_service->Initialize();
}

void SensorsProcessingService::Start() {
    const auto* sensors = sensors_configuration_manager_->Get();
    for(const auto& sensor : *sensors)
        InitializeScript(sensor);

    for(auto& processing_service : processing_services_)
        processing_service->Start();

    LOG_INF("Processing Service started.");
}

void SensorsProcessingService::Stop() {
    for(auto& processing_service : processing_services_)
        processing_service->Stop();

    sensor_readings_frame_->ClearReadings();

    LOG_INF("Processing Service stopped.");
}

void SensorsProcessingService::Pause() {
    for(auto& processing_service : processing_services_)
        processing_service->Pause();

    LOG_INF("Processing Service paused.");
}

void SensorsProcessingService::Resume() {
    for(auto& processing_service : processing_services_)
        processing_service->Resume();

    LOG_INF("Processing Service resumed.");
}

void SensorsProcessingService::RegisterReadingProcessor(std::shared_ptr<IReadingProcessor> processor) {
    reading_processors_->push_back(processor);
}

void SensorsProcessingService::InitializeScript(std::shared_ptr<Sensor> sensor) {
    auto lua_script = sensor->configuration.lua_script;

    if(lua_script == nullptr)
        return;

    GlobalFunctionsRegistry::RegisterGetSensorValue(*lua_script, *sensor_readings_frame_);
    GlobalFunctionsRegistry::RegisterUpdateSensorValue(*lua_script, *sensor_readings_frame_);
}

} // namespace eerie_leap::domain::sensor_domain::services
