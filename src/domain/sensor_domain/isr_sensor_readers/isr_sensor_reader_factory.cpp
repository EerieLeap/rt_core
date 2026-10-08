#include <zephyr/logging/log.h>

#include "domain/sensor_domain/models/sensor_type_traits.h"
#include "domain/sensor_domain/isr_sensor_readers/gpio_sensor_reader.h"

#include "isr_sensor_reader_factory.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_REGISTER(isr_sr_factory_logger);

IsrSensorReaderFactory::IsrSensorReaderFactory(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IGpio> gpio)
        : time_service_(std::move(time_service)),
        gpio_(std::move(gpio)) {}

std::unique_ptr<IIsrSensorReader> IsrSensorReaderFactory::Create(
    const SensorRuntime& runtime,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    ProcessSensorCallback process_sensor_callback) {

    const auto traits = runtime.GetSensor().configuration.GetTraits();

    try {
        switch(traits.source) {
        case SensorSourceKind::GPIO:
            if(gpio_ == nullptr)
                return nullptr;

            return std::make_unique<GpioSensorReader>(
                time_service_,
                runtime,
                std::move(process_sensor_callback),
                std::move(work_queue_thread),
                gpio_);

        default:
            return nullptr;
        }
    } catch (const std::runtime_error& e) {
        LOG_ERR("Failed to create ISR sensor reader: %s", e.what());
        return nullptr;
    }
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
