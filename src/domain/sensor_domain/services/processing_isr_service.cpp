#include "domain/sensor_domain/models/sensor_type_traits.h"

#include "processing_isr_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingIsrService::ProcessingIsrService(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<ReadingPipeline> pipeline)
        : time_service_(std::move(time_service)),
        isr_sensor_reader_factory_(std::move(isr_sensor_reader_factory)),
        canbus_service_(std::move(canbus_service)),
        work_queue_thread_(std::move(work_queue_thread)),
        pipeline_(std::move(pipeline)) {};

void ProcessingIsrService::SetGeneration(std::shared_ptr<const SensorGeneration> generation) {
    generation_ = std::move(generation);
}

// Runs on the processing work queue with the reading a source just produced.
void ProcessingIsrService::ProcessSensor(const SensorRuntime& runtime, SensorReading& reading) {
    pipeline_->Process(runtime, reading);

    if(reading.HasError())
        LOG_DBG("Sensor %s: %s", runtime.GetSensor().id.c_str(), ToString(reading.error).data());
}

bool ProcessingIsrService::DoStart() {
    if(generation_ == nullptr)
        return false;

    TearDown();

    const auto callback = [this](const SensorRuntime& runtime, SensorReading& reading) { ProcessSensor(runtime, reading); };

    if(canbus_service_ != nullptr) {
        can_frame_router_ = std::make_unique<CanFrameRouter>(time_service_, work_queue_thread_, canbus_service_, callback);

        const size_t routes = can_frame_router_->Attach(*generation_);
        LOG_INF("%zu CAN routes listening.", routes);
    }

    for(const auto& runtime : generation_->runtimes) {
        if(runtime.update_method != SensorReadingUpdateMethod::ISR
            || runtime.GetSensor().configuration.GetTraits().uses_canbus)
            continue;

        auto reader = isr_sensor_reader_factory_->Create(runtime, work_queue_thread_, callback);
        if(reader == nullptr)
            continue;

        readers_.push_back(std::move(reader));

        LOG_INF("Created ISR reader for sensor: %s", runtime.GetSensor().id.c_str());
    }

    return true;
}

// Sources go before the generation they refer to.
void ProcessingIsrService::TearDown() {
    can_frame_router_.reset();
    readers_.clear();
}

bool ProcessingIsrService::DoStop() {
    TearDown();
    generation_ = nullptr;

    return true;
}

bool ProcessingIsrService::DoPause() {
    TearDown();

    return true;
}

bool ProcessingIsrService::DoResume() {
    return DoStart();
}

} // namespace eerie_leap::domain::sensor_domain::services
