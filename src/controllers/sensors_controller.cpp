#include <string>
#include <vector>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/random/rng.h"

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"

#include "sensors_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::random;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(sensors_controller_logger);

SensorsController::SensorsController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
    std::shared_ptr<IFsService> sd_fs_service)
    : fs_service_(std::move(fs_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)),
      configuration_service_(std::move(configuration_service)),
      time_service_(std::move(time_service)),
      sensor_readings_frame_(std::move(sensor_readings_frame)),
      canbus_service_(std::move(canbus_service)),
      gpio_(std::move(gpio)),
      adc_configuration_manager_(std::move(adc_configuration_manager)),
      sd_fs_service_(std::move(sd_fs_service)) {}

SensorsController::~SensorsController() {
    if(restart_task_.has_value())
        restart_task_->Cancel();
}

int SensorsController::Initialize(const ConfigurationSetup& setup_test_configuration) {
    auto cbor_sensors_config_service = std::make_unique<config_services::CborConfigurationService<CborSensorsConfig>>(
        SENSORS_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_);
    sensors_configuration_manager_ = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_config_service),
        sd_fs_service_,
        gpio_ != nullptr ? gpio_->GetChannelCount() : 0,
        adc_configuration_manager_ != nullptr ? adc_configuration_manager_->Get()->GetChannelCount() : 0);

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Sensors, sensors_configuration_manager_);

    isr_sensor_reader_factory_ = std::make_shared<IsrSensorReaderFactory>(
        time_service_,
        gpio_);

    // Polled sensors need no ADC unless they are PHYSICAL_ANALOG; the factory checks per sensor.
    sensor_reader_factory_ = std::make_shared<SensorReaderFactory>(
        time_service_,
        gpio_,
        adc_configuration_manager_);

    sensors_processing_service_ = std::make_shared<SensorsProcessingService>(
        sensors_configuration_manager_,
        sensor_readings_frame_,
        time_service_,
        isr_sensor_reader_factory_,
        sensor_reader_factory_,
        canbus_service_,
        sd_fs_service_);
    if(!sensors_processing_service_->Initialize()) {
        LOG_ERR("Failed to initialize the sensors processing service.");
        return -1;
    }

    // TODO: For test purposes only
    if(setup_test_configuration)
        setup_test_configuration(sensors_configuration_manager_);

    restart_task_ = config_work_queue_thread_->CreateTask(
        [](SensorsController* controller) {
            controller->RestartProcessing();
            return WorkQueueTaskResult{};
        }, this);

    // Registered last so the test configuration above does not trigger a restart. The handler runs on
    // the configuration work queue while the SMP apply job holds its lock, so the restart is queued behind it.
    sensors_configuration_manager_->RegisterConfigurationUpdatedHandler([this] {
        restart_task_->Reschedule(K_NO_WAIT);
    });

    return 0;
}

int SensorsController::Start() {
    return sensors_processing_service_->Start() ? 0 : -1;
}

// Readers and tasks are built from the configuration in DoStart(), so a stored configuration
// only takes effect through a restart.
void SensorsController::RestartProcessing() {
    if(sensors_processing_service_->IsStopped())
        return;

    if(!sensors_processing_service_->Stop()) {
        LOG_ERR("Failed to stop the sensors processing service for the new configuration.");
        return;
    }

    if(!sensors_processing_service_->Start())
        LOG_ERR("Failed to start the sensors processing service with the new configuration.");
}

void SensorsController::EmulateReadings() {
    const auto sensors = sensors_configuration_manager_->Get();
    if(sensors == nullptr)
        return;

    for(const auto& sensor : *sensors) {
        SensorReading reading(sensor.get());
        reading.source = ReadingSource::PROCESSING;
        reading.status = ReadingStatus::PROCESSED;
        reading.value = (Rng::Get<uint32_t>() / static_cast<float>(UINT32_MAX)) * 100.0F;

        sensor_readings_frame_->AddOrUpdateReading(reading);
    }
}

} // namespace eerie_leap::controllers
