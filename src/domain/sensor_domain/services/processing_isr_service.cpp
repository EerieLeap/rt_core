#include <span>

#include "subsys/time/time_helpers.hpp"
#include "subsys/lua_script/lua_script.h"
#include "domain/script_domain/utilities/global_fuctions_registry.h"

#include "processing_isr_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::lua_script;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::script_domain::utilities;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingIsrService::ProcessingIsrService(
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<IsrSensorReaderFactory> isr_sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors)
        : sensors_configuration_manager_(std::move(sensors_configuration_manager)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        isr_sensor_reader_factory_(std::move(isr_sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        reading_processors_(std::move(reading_processors)) {

    collect_isr_reading_processor_ = std::make_unique<CollectIsrReadingProcessor>(sensor_readings_frame_);
};

void ProcessingIsrService::ProcessSensor(const Sensor& sensor) {
    try {
        if(sensor_readings_frame_->HasIsrReading(sensor.id_hash)) {
            collect_isr_reading_processor_->ProcessReading(sensor.id_hash);
            for(auto processor : *reading_processors_)
                processor->ProcessReading(sensor.id_hash);

            auto reading_optioanl = sensor_readings_frame_->TryGetReading(sensor.id_hash);
            if(!reading_optioanl)
                return;
            auto reading = std::move(reading_optioanl.value());

            if(reading.status < ReadingStatus::PROCESSED) {
                reading.status = ReadingStatus::PROCESSED;
                sensor_readings_frame_->AddOrUpdateReading(reading);
            }

            LOG_DBG("Sensor Reading - ID: %s, Guid: %llu, Value: %.3f, Time: %s",
                sensor.id.c_str(),
                reading.id.AsUint64(),
                reading.value.value_or(0.0f),
                TimeHelpers::GetFormattedString(reading.timestamp.value()).c_str());
        }
    } catch (const std::exception& e) {
        LOG_DBG("Error processing sensor: %s, Error: %s", sensor.id.c_str(), e.what());
    }
}

bool ProcessingIsrService::DoStart() {
    const auto* sensors = sensors_configuration_manager_->Get();

    readers_.clear();
    for(const auto& sensor : *sensors) {
        if(sensor->configuration.GetReadingUpdateMethod() != SensorReadingUpdateMethod::ISR)
            continue;

        auto reader = isr_sensor_reader_factory_->Create(
            sensor,
            work_queue_thread_,
            [this](const Sensor& sensor) { ProcessSensor(sensor); });

        if(reader == nullptr)
            continue;

        readers_.push_back(std::move(reader));

        LOG_INF("Created ISR reader for sensor: %s", sensor->id.c_str());
    }

    return true;
}

bool ProcessingIsrService::DoStop() {
    readers_.clear();

    return true;
}

bool ProcessingIsrService::DoPause() {
    readers_.clear();

    return true;
}

bool ProcessingIsrService::DoResume() {
    return DoStart();
}

} // namespace eerie_leap::domain::sensor_domain::services
