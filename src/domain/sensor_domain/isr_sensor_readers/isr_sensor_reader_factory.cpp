#include <zephyr/logging/log.h>

#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/isr_sensor_readers/canbus_sensor_reader_raw.h"
#include "domain/sensor_domain/isr_sensor_readers/canbus_sensor_reader.h"
#include "domain/sensor_domain/isr_sensor_readers/gpio_sensor_reader.h"

#include "isr_sensor_reader_factory.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_REGISTER(isr_sr_factory_logger);

IsrSensorReaderFactory::IsrSensorReaderFactory(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<IGpio> gpio)
        : time_service_(std::move(time_service)),
        guid_generator_(std::move(guid_generator)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        canbus_service_(std::move(canbus_service)),
        gpio_(std::move(gpio)) {}

std::unique_ptr<IIsrSensorReader> IsrSensorReaderFactory::Create(
    std::shared_ptr<Sensor> sensor,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    ProcessSensorCallback process_sensor_callback) {

    std::unique_ptr<IIsrSensorReader> sensor_reader;

    try {
        if(sensor->configuration.type == SensorType::CANBUS_RAW) {
            auto canbus = canbus_service_->GetCanbus(sensor->configuration.canbus_source->bus_channel);
            if(canbus == nullptr)
                return nullptr;

            sensor_reader = std::make_unique<CanbusSensorReaderRaw>(
                time_service_,
                guid_generator_,
                sensor_readings_frame_,
                sensor,
                std::move(process_sensor_callback),
                std::move(work_queue_thread),
                canbus);
        } else if(sensor->configuration.type == SensorType::CANBUS_ANALOG || sensor->configuration.type == SensorType::CANBUS_INDICATOR) {
            auto canbus = canbus_service_->GetCanbus(sensor->configuration.canbus_source->bus_channel);
            if(canbus == nullptr)
                return nullptr;

            auto message_configuration = canbus_service_->GetMessageConfiguration(
                sensor->configuration.canbus_source->bus_channel,
                sensor->configuration.canbus_source->frame_id);

            if(message_configuration == nullptr)
                return nullptr;

            const auto* signal_configuration = message_configuration->TryGetSignal(
                sensor->configuration.canbus_source->signal_name_hash);

            if(signal_configuration == nullptr)
                return nullptr;

            sensor_reader = std::make_unique<CanbusSensorReader>(
                time_service_,
                guid_generator_,
                sensor_readings_frame_,
                sensor,
                std::move(process_sensor_callback),
                std::move(work_queue_thread),
                canbus,
                std::shared_ptr<const CanSignalConfiguration>(std::move(message_configuration), signal_configuration));
        } else if(sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
            if(gpio_ == nullptr)
                return nullptr;

            sensor_reader = std::make_unique<GpioSensorReader>(
                time_service_,
                guid_generator_,
                sensor_readings_frame_,
                sensor,
                std::move(process_sensor_callback),
                std::move(work_queue_thread),
                gpio_);
        } else {
            return nullptr;
        }
    } catch (const std::runtime_error& e) {
        LOG_ERR("Failed to create ISR sensor reader: %s", e.what());
        return nullptr;
    }

    return sensor_reader;
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
