#include <zephyr/logging/log.h>

#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/isr_sensor_readers/canbus_sensor_reader_raw.h"
#include "domain/sensor_domain/isr_sensor_readers/canbus_sensor_reader.h"

#include "isr_sensor_reader_factory.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

LOG_MODULE_REGISTER(isr_sr_factory_logger);

IsrSensorReaderFactory::IsrSensorReaderFactory(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<CanbusService> canbus_service)
        : time_service_(std::move(time_service)),
        guid_generator_(std::move(guid_generator)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        canbus_service_(std::move(canbus_service)) {}

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

            const auto* channel_configuration = canbus_service_->GetChannelConfiguration(sensor->configuration.canbus_source->bus_channel);
            if(channel_configuration == nullptr)
                return nullptr;

            if(!channel_configuration->dbc->HasMessage(sensor->configuration.canbus_source->frame_id))
                return nullptr;
            auto* dbc_message = channel_configuration->dbc->GetMessage(
                sensor->configuration.canbus_source->frame_id);

            if(!dbc_message->HasSignal(sensor->configuration.canbus_source->signal_name))
                return nullptr;

            sensor_reader = std::make_unique<CanbusSensorReader>(
                time_service_,
                guid_generator_,
                sensor_readings_frame_,
                sensor,
                std::move(process_sensor_callback),
                std::move(work_queue_thread),
                canbus,
                channel_configuration->dbc);
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
