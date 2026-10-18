#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <eerie_memory.hpp>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/device_tree/dt_canbus.h"

#include "domain/canbus_domain/models/canbus_configuration.h"

#ifdef CONFIG_EERIE_LEAP_ASSETS
    #include "subsys/assets/asset_kind.h"
    #include "subsys/assets/asset_store.h"
    #include "subsys/assets/assets_manager.h"
    #include "subsys/assets/script_asset_provider.h"
    #include "utilities/string/path_helpers.h"
#endif

#include "canbus_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::canbus_domain::models;
using eerie_leap::subsys::threading::WorkQueueTaskResult;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(canbus_controller_logger);

#ifdef CONFIG_EERIE_LEAP_ASSETS
namespace {

// Relative to the SD volume. The CAN scripts may also live elsewhere on it: a message's script_path
// names any file there, and a script of the sensor scripts' directory is referred to just the same.
constexpr const char* CAN_SCRIPTS_DIRECTORY = "scripts/can";

// Whether the CAN configuration of @p manager has a message, enabled or not, on any channel that uses
// the script at @p path. Holds the manager weakly, since the registry may outlive the controller. Runs
// on the configuration work queue.
eerie_leap::subsys::assets::AssetsManager::Referrer
MakeCanScriptReferrer(const std::shared_ptr<CanbusConfigurationManager>& manager) {

    return [weak_manager = std::weak_ptr<CanbusConfigurationManager>(manager)](std::string_view path) -> bool {
        const auto canbus_manager = weak_manager.lock();
        if(canbus_manager == nullptr)
            return false;

        try {
            const auto configuration = canbus_manager->Get();
            if(configuration == nullptr)
                return false;

            for(const auto& [channel_id, channel] : configuration->channel_configurations) {
                for(const auto& message : channel.message_configurations) {
                    if(message != nullptr
                        && eerie_leap::utilities::string::PathHelpers::IsSameEntryIgnoringCase(
                            path, message->script_path
                        ))
                        return true;
                }
            }

            return false;
        } catch(const std::exception& e) {
            // Unknown is answered as referenced: a script that might be in use is not deleted.
            LOG_ERR("Failed to read the CAN configuration for its scripts: %s", e.what());

            return true;
        }
    };
}

} // namespace
#endif

CanbusController::CanbusController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<AssetsManager> assets_manager
) :
    fs_service_(std::move(fs_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    configuration_service_(std::move(configuration_service)),
    sd_fs_service_(std::move(sd_fs_service)),
    assets_manager_(std::move(assets_manager)) {}

CanbusController::~CanbusController() {
    // First, so that no request reaches the scripts, or the configuration, of a controller on its way out.
    UnregisterAssets();

    if(reconfigure_task_.has_value())
        reconfigure_task_->Cancel();
}

int CanbusController::Initialize(const ConfigurationSetup& setup_test_configuration) {
    auto cbor_canbus_config_service = std::make_unique<config_services::CborConfigurationService<CborCanbusConfig>>(
        CANBUS_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
    );
    canbus_configuration_manager_ = std::make_shared<CanbusConfigurationManager>(
        std::move(cbor_canbus_config_service), sd_fs_service_
    );

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Canbus, canbus_configuration_manager_
        );

    // TODO: For test purposes only
    if(setup_test_configuration)
        setup_test_configuration(canbus_configuration_manager_);

    canbus_service_ = std::make_shared<CanbusService>(
        DtCanbus::Get, canbus_configuration_manager_, config_work_queue_thread_
    );

    canbus_com_service_ = std::make_shared<CanbusComService>(canbus_service_);
    if(!canbus_com_service_->Initialize()) {
        LOG_ERR("Failed to initialize the CANBus COM service.");
        return -1;
    }

    network_mgmt_group_ = std::make_unique<NetworkMgmtGroup>(canbus_com_service_->GetNetworkInfo());
    network_mgmt_group_->Register();

    reconfigure_task_ = config_work_queue_thread_->CreateTask(
        [](CanbusController* controller) {
            controller->Reconfigure();
            return WorkQueueTaskResult{};
        },
        this
    );

    // Registered last so the test configuration above does not trigger a reconfiguration.
    canbus_configuration_manager_->RegisterConfigurationUpdatedHandler([this] {
        // Restarting the CAN bus services drops what the SMP CAN transport has yet to send, so the
        // request that stored the configuration, which may have come over the bus, is answered first.
        reconfigure_task_->Reschedule(K_MSEC(CONFIG_EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS));
    });

    // After the last point that can fail.
    RegisterAssets();

    return 0;
}

void CanbusController::RegisterAssets() {
#ifdef CONFIG_EERIE_LEAP_ASSETS
    using namespace eerie_leap::subsys::assets;

    if(assets_manager_ == nullptr || sd_fs_service_ == nullptr)
        return;

    try {
        auto store = std::make_shared<AssetStore>(sd_fs_service_, CAN_SCRIPTS_DIRECTORY);

        // The referrer comes first, so that the kind is never served without it.
        assets_manager_->AddReferrer(this, MakeCanScriptReferrer(canbus_configuration_manager_));

        is_assets_registered_ = assets_manager_->Register(MakeScriptProvider(
            AssetKind::CAN_SCRIPTS, "can_scripts", std::move(store), CONFIG_EERIE_LEAP_CAN_SCRIPT_MAX_SIZE,
            assets_manager_
        ));

        if(!is_assets_registered_)
            assets_manager_->RemoveReferrers(this);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to register the CAN scripts as assets: %s", e.what());

        assets_manager_->RemoveReferrers(this);
    }
#endif
}

void CanbusController::UnregisterAssets() {
#ifdef CONFIG_EERIE_LEAP_ASSETS
    if(assets_manager_ == nullptr)
        return;

    // The kind first: while it is served, its referrer stays.
    if(is_assets_registered_) {
        assets_manager_->Unregister(eerie_leap::subsys::assets::AssetKind::CAN_SCRIPTS);
        is_assets_registered_ = false;
    }

    assets_manager_->RemoveReferrers(this);
#endif
}

int CanbusController::Start() {
    return canbus_com_service_->Start() ? 0 : -1;
}

void CanbusController::RegisterDependentService(std::shared_ptr<IService> service) {
    if(service != nullptr)
        dependent_services_.push_back(std::move(service));
}

void CanbusController::Reconfigure() {
    canbus_com_service_->Stop();
    for(const auto& service : dependent_services_)
        service->Stop();

    canbus_service_->Configure();

    canbus_com_service_->Start();
    for(const auto& service : dependent_services_)
        service->Start();
}

} // namespace eerie_leap::controllers
