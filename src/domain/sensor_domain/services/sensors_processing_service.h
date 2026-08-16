#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"

#include "sensor_task.hpp"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;

class SensorsProcessingService final : public ServiceBase<> {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory_;
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory_;

    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors_;
    std::vector<std::unique_ptr<IService>> processing_services_;

    void InitializeScript(std::shared_ptr<Sensor> sensor) const;

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    SensorsProcessingService(
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
        std::shared_ptr<SensorReaderFactory> sensor_reader_factory);

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }

    void RegisterReadingProcessor(std::shared_ptr<IReadingProcessor> processor) const;
};

} // namespace eerie_leap::domain::sensor_domain::services
