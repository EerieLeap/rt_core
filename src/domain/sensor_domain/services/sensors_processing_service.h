#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::adc_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::processors::IndicatorSwitchState;
using eerie_leap::domain::sensor_domain::processors::IReadingProcessor;
using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::runtime::SensorPipelineBuilder;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;

class ProcessingIsrService;
class ProcessingSchedulerService;

// Builds a generation from the current sensors and ADC configurations on Start() and runs it until
// Stop(); a new configuration of either takes effect through a restart.
class SensorsProcessingService final : public ServiceBase<> {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    SensorPipelineBuilder pipeline_builder_;

    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;
    std::shared_ptr<ReadingPipeline> pipeline_;
    std::shared_ptr<ProcessingIsrService> isr_service_;
    std::shared_ptr<ProcessingSchedulerService> scheduler_service_;
    std::vector<std::shared_ptr<IService>> processing_services_;

    std::shared_ptr<SensorGeneration> generation_;

    // The switch of every Toggle indicator of the last generation that ran, for the next start.
    // A failed start keeps them; a reboot loses them.
    std::vector<IndicatorSwitchState> switch_states_;

    struct SwitchStateWork {
        k_work work;
        SensorsProcessingService* service;
    };

    // Commits the generation's switches on the processing work queue, where their conditioners run.
    SwitchStateWork switch_state_work_{};
    k_work_sync switch_state_sync_{};

    static void SwitchStateWorkHandler(k_work* work);
    void CommitSwitchStates();

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    SensorsProcessingService(
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        const std::shared_ptr<ITimeService>& time_service,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
        // Snapshotted into every generation; PHYSICAL_ANALOG sensors get no reader without it.
        std::shared_ptr<AdcConfigurationManager> adc_configuration_manager = nullptr,
        std::shared_ptr<CanbusService> canbus_service = nullptr,
        std::shared_ptr<IFsService> sd_fs_service = nullptr
    );

    [[nodiscard]] bool IsPausable() const noexcept override {
        return true;
    }

    // Only while the service is stopped; returns false otherwise.
    bool RegisterReadingProcessor(std::shared_ptr<IReadingProcessor> processor);

    /** @return The generation currently running, or null while stopped. */
    [[nodiscard]] std::shared_ptr<const SensorGeneration> GetGeneration() const {
        return generation_;
    }
};

} // namespace eerie_leap::domain::sensor_domain::services
