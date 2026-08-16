#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"
#include "domain/sensor_domain/processors/collect_isr_reading_processor.h"

#include "sensor_task.hpp"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IIsrSensorReader;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::processors::CollectIsrReadingProcessor;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;

class ProcessingIsrService final : public ServiceBase<> {
private:
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory_;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::unique_ptr<CollectIsrReadingProcessor> collect_isr_reading_processor_;
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;
    std::vector<std::unique_ptr<IIsrSensorReader>> readers_;

    void ProcessSensor(const Sensor& sensor);

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingIsrService(
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }
};

} // namespace eerie_leap::domain::sensor_domain::services
