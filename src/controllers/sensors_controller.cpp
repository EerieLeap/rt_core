#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/random/rng.h"

#ifdef CONFIG_EERIE_LEAP_ASSETS
    #include "subsys/assets/asset_kind.h"
    #include "subsys/assets/asset_store.h"
    #include "subsys/assets/assets_manager.h"
    #include "subsys/assets/script_asset_provider.h"
    #include "utilities/string/path_helpers.h"
#endif

#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_type.h"
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

#ifdef CONFIG_EERIE_LEAP_ASSETS
namespace {

// Relative to the SD volume. The sensor scripts may also live elsewhere on it: a sensor's script_path
// names any file there, and a script of the CAN scripts' directory is referred to just the same.
constexpr const char* SENSOR_SCRIPTS_DIRECTORY = "scripts/sensors";

// Whether the sensors configuration of @p manager has a sensor, enabled or not, that uses the script
// at @p path. Holds the manager weakly, since the registry may outlive the controller. Runs on the
// configuration work queue.
eerie_leap::subsys::assets::AssetsManager::Referrer
MakeSensorScriptReferrer(const std::shared_ptr<SensorsConfigurationManager>& manager) {

    return [weak_manager = std::weak_ptr<SensorsConfigurationManager>(manager)](std::string_view path) -> bool {
        const auto sensors_manager = weak_manager.lock();
        if(sensors_manager == nullptr)
            return false;

        try {
            const auto sensors = sensors_manager->Get();
            if(sensors == nullptr)
                return false;

            return std::ranges::any_of(*sensors, [path](const std::shared_ptr<Sensor>& sensor) {
                return eerie_leap::utilities::string::PathHelpers::IsSameEntryIgnoringCase(
                    path, sensor->configuration.script_path
                );
            });
        } catch(const std::exception& e) {
            // Unknown is answered as referenced: a script that might be in use is not deleted.
            LOG_ERR("Failed to read the sensors configuration for its scripts: %s", e.what());

            return true;
        }
    };
}

} // namespace
#endif

SensorsController::SensorsController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<CanbusService> canbus_service,
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<IAdcManager> adc_manager,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<AssetsManager> assets_manager
) :
    fs_service_(std::move(fs_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    configuration_service_(std::move(configuration_service)),
    time_service_(std::move(time_service)),
    sensor_readings_frame_(std::move(sensor_readings_frame)),
    canbus_service_(std::move(canbus_service)),
    gpio_(std::move(gpio)),
    adc_manager_(std::move(adc_manager)),
    adc_configuration_manager_(std::move(adc_configuration_manager)),
    sd_fs_service_(std::move(sd_fs_service)),
    assets_manager_(std::move(assets_manager)) {}

SensorsController::~SensorsController() {
    // First, so that no request reaches the scripts, or the configuration, of a controller on its way out.
    UnregisterAssets();

    // The ADC configuration manager is not ours and may outlive the handler's capture.
    if(adc_configuration_manager_ != nullptr)
        adc_configuration_manager_->RegisterConfigurationUpdatedHandler(nullptr);

    if(restart_task_.has_value())
        restart_task_->Cancel();
}

int SensorsController::Initialize(const ConfigurationSetup& setup_test_configuration) {
    auto cbor_sensors_config_service = std::make_unique<config_services::CborConfigurationService<CborSensorsConfig>>(
        SENSORS_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
    );

    // A client's CAN sensors are checked against the CAN bus configuration in use.
    SensorsConfigurationManager::CanbusConfigurationProvider canbus_configuration_provider = nullptr;
    if(canbus_service_ != nullptr)
        canbus_configuration_provider = [canbus_service = canbus_service_] {
            return canbus_service->GetConfiguration();
        };

    sensors_configuration_manager_ = std::make_shared<SensorsConfigurationManager>(
        std::move(cbor_sensors_config_service), sd_fs_service_, gpio_ != nullptr ? gpio_->GetChannelCount() : 0,
        adc_manager_ != nullptr ? adc_manager_->GetChannelCount() : 0, std::move(canbus_configuration_provider)
    );

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Sensors, sensors_configuration_manager_
        );

    isr_sensor_reader_factory_ = std::make_shared<IsrSensorReaderFactory>(time_service_, gpio_);

    // Polled sensors need no ADC unless they are PHYSICAL_ANALOG; the factory checks per sensor.
    sensor_reader_factory_ = std::make_shared<SensorReaderFactory>(time_service_, gpio_, adc_manager_);

    sensors_processing_service_ = std::make_shared<SensorsProcessingService>(
        sensors_configuration_manager_, sensor_readings_frame_, time_service_, isr_sensor_reader_factory_,
        sensor_reader_factory_, adc_configuration_manager_, canbus_service_, sd_fs_service_
    );
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
        },
        this
    );

    // Registered last so the test configuration above does not trigger a restart. The handler runs on
    // the configuration work queue while the SMP apply job holds its lock, so the restart is queued behind it.
    sensors_configuration_manager_->RegisterConfigurationUpdatedHandler([this] {
        restart_task_->Reschedule(K_NO_WAIT);
    });

    // A generation snapshots the ADC configuration when it is built, so a new one takes effect
    // through the same restart.
    if(adc_configuration_manager_ != nullptr)
        adc_configuration_manager_->RegisterConfigurationUpdatedHandler([this] {
            restart_task_->Reschedule(K_NO_WAIT);
        });

    // After the last point that can fail.
    RegisterAssets();

    return 0;
}

void SensorsController::RegisterAssets() {
#ifdef CONFIG_EERIE_LEAP_ASSETS
    using namespace eerie_leap::subsys::assets;

    if(assets_manager_ == nullptr || sd_fs_service_ == nullptr)
        return;

    try {
        auto store = std::make_shared<AssetStore>(sd_fs_service_, SENSOR_SCRIPTS_DIRECTORY);

        // The referrer comes first, so that the kind is never served without it.
        assets_manager_->AddReferrer(this, MakeSensorScriptReferrer(sensors_configuration_manager_));

        is_assets_registered_ = assets_manager_->Register(MakeScriptProvider(
            AssetKind::SENSOR_SCRIPTS, "sensor_scripts", std::move(store), CONFIG_EERIE_LEAP_SENSOR_SCRIPT_MAX_SIZE,
            assets_manager_
        ));

        if(!is_assets_registered_)
            assets_manager_->RemoveReferrers(this);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to register the sensor scripts as assets: %s", e.what());

        assets_manager_->RemoveReferrers(this);
    }
#endif
}

void SensorsController::UnregisterAssets() {
#ifdef CONFIG_EERIE_LEAP_ASSETS
    if(assets_manager_ == nullptr)
        return;

    // The kind first: while it is served, its referrer stays.
    if(is_assets_registered_) {
        assets_manager_->Unregister(eerie_leap::subsys::assets::AssetKind::SENSOR_SCRIPTS);
        is_assets_registered_ = false;
    }

    assets_manager_->RemoveReferrers(this);
#endif
}

int SensorsController::Start() {
    is_processing_requested_ = true;

    try {
        return sensors_processing_service_->Start() ? 0 : -1;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to start the sensors processing service: %s", e.what());
        return -1;
    }
}

SensorCapabilities SensorsController::GetCapabilities() const {
    return {
        .adc_channels = static_cast<uint32_t>(sensors_configuration_manager_->GetAdcChannelCount()),
        .gpio_channels = static_cast<uint32_t>(sensors_configuration_manager_->GetGpioChannelCount()),
        .sensors_max = CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT,
    };
}

void SensorsController::RegisterDependentService(std::shared_ptr<IService> service) {
    if(service != nullptr)
        dependent_services_.push_back(std::move(service));
}

// Readers and tasks are built from the configuration in DoStart(), so a stored configuration
// only takes effect through a restart. Before Start() there is nothing to restart; after a failed
// start or restart the service is stopped, and a new configuration is the way out of it.
void SensorsController::RestartProcessing() {
    if(!is_processing_requested_)
        return;

    // The dependents stop before the generation they read goes, and start once the new one runs.
    std::vector<std::shared_ptr<IService>> stopped_services;
    for(const auto& service : dependent_services_) {
        if(!service->IsRunning())
            continue;

        try {
            if(service->Stop())
                stopped_services.push_back(service);
            else
                LOG_ERR("Failed to stop a dependent service for the new configuration.");
        } catch(const std::exception& e) {
            LOG_ERR("Failed to stop a dependent service for the new configuration: %s", e.what());
        }
    }

    try {
        if(!sensors_processing_service_->Stop()) {
            LOG_ERR("Failed to stop the sensors processing service for the new configuration.");
        } else if(!sensors_processing_service_->Start()) {
            LOG_ERR("Failed to start the sensors processing service with the new configuration.");
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to restart the sensors processing service: %s", e.what());
    }

    // Whatever processing came back with: a dependent without readings waits for them, as at boot.
    for(const auto& service : stopped_services) {
        try {
            if(!service->Start())
                LOG_ERR("Failed to restart a dependent service after the new configuration.");
        } catch(const std::exception& e) {
            LOG_ERR("Failed to restart a dependent service after the new configuration: %s", e.what());
        }
    }
}

void SensorsController::EmulateReadings() {
    const auto sensors = sensors_configuration_manager_->Get();
    if(sensors == nullptr)
        return;

    for(const auto& sensor : *sensors) {
        if(!sensor->is_enabled)
            continue;

        SensorReading reading(sensor.get());
        reading.source = ReadingSource::PROCESSING;
        reading.status = ReadingStatus::PROCESSED;
        reading.value = (static_cast<float>(Rng::Get<uint32_t>()) / static_cast<float>(UINT32_MAX)) * 100.0F;

        sensor_readings_frame_->AddOrUpdateReading(reading);
    }
}

} // namespace eerie_leap::controllers
