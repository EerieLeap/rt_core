#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"

#include "sensor_task.hpp"
#include "i_sensors_processing_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;

class SensorsProcessingService : public ISensorsProcessingService {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory_;
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory_;

    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;
    std::vector<std::unique_ptr<ISensorsProcessingService>> processing_services_;

    void InitializeScript(std::shared_ptr<Sensor> sensor);

public:
    SensorsProcessingService(
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory);
    ~SensorsProcessingService() = default;

    void Initialize() override;
    void Start() override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    void RegisterReadingProcessor(std::shared_ptr<IReadingProcessor> processor);
};

} // namespace eerie_leap::domain::sensor_domain::services
