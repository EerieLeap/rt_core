#include <algorithm>
#include <exception>
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
    const std::shared_ptr<ITimeService>& time_service,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<IFsService> sd_fs_service
) :
    work_queue_thread_(nullptr),
    sensors_configuration_manager_(std::move(sensors_configuration_manager)),
    adc_configuration_manager_(std::move(adc_configuration_manager)),
    sensor_readings_frame_(std::move(sensor_readings_frame)),
    pipeline_builder_(std::move(sd_fs_service), sensor_readings_frame_),
    reading_processors_(std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>()) {

    work_queue_thread_ = std::make_shared<WorkQueueThread>(
        "processing_service", CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY
    );

    reading_processors_->push_back(std::make_shared<ExpressionProcessor>(sensor_readings_frame_));
    reading_processors_->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value"));

    pipeline_ = std::make_shared<ReadingPipeline>(
        sensor_readings_frame_, reading_processors_, time_service, work_queue_thread_
    );

    start_values_work_.service = this;
    k_work_init(&start_values_work_.work, StartValuesWorkHandler);

    if(isr_sensor_reader_factory != nullptr || canbus_service != nullptr) {
        isr_service_ = std::make_shared<ProcessingIsrService>(
            time_service, std::move(isr_sensor_reader_factory), std::move(canbus_service), work_queue_thread_, pipeline_
        );
        processing_services_.push_back(isr_service_);
    }

    if(sensor_reader_factory != nullptr) {
        scheduler_service_ = std::make_shared<ProcessingSchedulerService>(
            time_service, std::move(sensor_reader_factory), work_queue_thread_, pipeline_
        );
        processing_services_.push_back(scheduler_service_);
    }
};

bool SensorsProcessingService::DoInitialize() {
    if(!work_queue_thread_->Initialize()) {
        LOG_ERR("Failed to initialize the processing work queue.");
        return false;
    }

    // Stops at the first service that fails to initialize.
    return std::ranges::all_of(processing_services_, [](const auto& processing_service) {
        return processing_service->Initialize();
    });
}

bool SensorsProcessingService::DoStart() {
    const auto sensors = sensors_configuration_manager_->Get();
    if(sensors == nullptr) {
        LOG_ERR("No sensors configuration available.");
        return false;
    }

    // Everything a sample needs is allocated here, once per configuration. The ADC configuration is
    // taken as it is now; an apply swaps the manager's pointer and restarts, never touching this one.
    try {
        generation_ = pipeline_builder_.Build(sensors, switch_states_);
        generation_->adc_configuration = adc_configuration_manager_ != nullptr ? adc_configuration_manager_->Get()
                                                                               : nullptr;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to build the sensor pipeline: %s", e.what());
        generation_ = nullptr;
        return false;
    }

    if(isr_service_ != nullptr)
        isr_service_->SetGeneration(generation_);
    if(scheduler_service_ != nullptr)
        scheduler_service_->SetGeneration(generation_);

    // Nothing may keep running on a generation that is being dropped. The child that threw has
    // cleaned up after itself; DoStop() stops the ones that started before it.
    try {
        // A switch is known before its input, and a control indicator starts released: the frame, the
        // log and the publisher see them at once. On the processing work queue, ahead of every source
        // started below.
        CommitStartValues();

        for(const auto& processing_service : processing_services_)
            processing_service->Start();
    } catch(...) {
        // The generation never ran, so the next start takes the switches of the last one that did.
        auto switch_states = std::move(switch_states_);
        DoStop();
        switch_states_ = std::move(switch_states);

        if(isr_service_ != nullptr)
            isr_service_->SetGeneration(nullptr);
        if(scheduler_service_ != nullptr)
            scheduler_service_->SetGeneration(nullptr);

        throw;
    }

    LOG_INF(
        "Processing Service started with generation %u, %zu sensors.", generation_->id, generation_->runtimes.size()
    );

    return true;
}

bool SensorsProcessingService::DoStop() {
    for(const auto& processing_service : processing_services_)
        processing_service->Stop();

    // The sources are gone, but a held indicator change could still settle into the generation.
    // The switches go to the next one.
    if(generation_ != nullptr) {
        generation_->CancelSettles();
        switch_states_ = generation_->GetSwitchStates();
    }

    // Nothing refers to the generation any more.
    sensor_readings_frame_->ClearReadings();
    generation_ = nullptr;

    LOG_INF("Processing Service stopped.");

    return true;
}

bool SensorsProcessingService::DoPause() {
    for(const auto& processing_service : processing_services_)
        processing_service->Pause();

    // No source can confirm or cancel a held indicator change until Resume(), so its window stops
    // with them: a bounce or a short press in flight must not settle unobserved.
    if(generation_ != nullptr)
        generation_->SuspendSettles();

    LOG_INF("Processing Service paused.");

    return true;
}

bool SensorsProcessingService::DoResume() {
    // Ahead of the sources, so their first readings, a GPIO level read again or the next sample,
    // confirm or cancel a held change; one that sends nothing more settles at the window's end.
    if(generation_ != nullptr)
        generation_->ResumeSettles();

    for(const auto& processing_service : processing_services_)
        processing_service->Resume();

    LOG_INF("Processing Service resumed.");

    return true;
}

void SensorsProcessingService::CommitStartValues() {
    if(!generation_->HasStartValues())
        return;

    // The starting thread waits; it is never the processing work queue, and nothing that queue
    // runs takes the transition lock the starter holds.
    if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &start_values_work_.work) < 0)
        throw std::runtime_error("The processing work queue refused the start values.");

    k_work_flush(&start_values_work_.work, &start_values_sync_);
}

// Exceptions must not unwind into the work queue's C dispatch.
void SensorsProcessingService::StartValuesWorkHandler(k_work* work) {
    const auto* start_values_work = CONTAINER_OF(work, StartValuesWork, work);
    const auto* service = start_values_work->service;

    try {
        service->pipeline_->CommitStartValues(*service->generation_);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to commit the start values: %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to commit the start values.");
    }
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
