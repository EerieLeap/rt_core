#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IIsrSensorReader;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::processors::IReadingProcessor;

// Runs the event-driven sensors of a generation.
class ProcessingIsrService final : public ServiceBase<> {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory_;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;
    std::shared_ptr<const SensorGeneration> generation_;
    std::vector<std::unique_ptr<IIsrSensorReader>> readers_;

    void ProcessSensor(const SensorRuntime& runtime, SensorReading& reading);

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingIsrService(
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors);

    // The generation to run; set while stopped, before Start().
    void SetGeneration(std::shared_ptr<const SensorGeneration> generation);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }
};

} // namespace eerie_leap::domain::sensor_domain::services
