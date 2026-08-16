#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"

#include "sensor_task.hpp"

namespace eerie_leap::domain::sensor_domain::services {

namespace threading = eerie_leap::subsys::threading;

using threading::ServiceBase;
using threading::ServiceState;
using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;

class ProcessingSchedulerService final : public ServiceBase<> {
private:
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory_;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::vector<threading::WorkQueueTask<SensorTask>> work_queue_tasks_;

    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;

    void StartTasks();
    void CancelTasks();
    std::unique_ptr<SensorTask> CreateSensorTask(std::shared_ptr<Sensor> sensor);
    static WorkQueueTaskResult ProcessSensorWorkTask(SensorTask* task);

    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    ProcessingSchedulerService(
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }
};

} // namespace eerie_leap::domain::sensor_domain::services
