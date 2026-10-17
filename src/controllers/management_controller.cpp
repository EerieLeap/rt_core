#include <utility>

#include <zephyr/logging/log.h>

#include "management_controller.h"

namespace eerie_leap::controllers {

LOG_MODULE_REGISTER(management_controller_logger);

ManagementController::ManagementController(
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<const ICdmpNetworkInfo> network_info,
    uint32_t build_number,
    std::optional<SensorCapabilities> sensor_capabilities,
    std::shared_ptr<const IFsService> sd_fs_service,
    [[maybe_unused]] std::shared_ptr<AssetsManager> assets_manager)
    : configuration_service_(std::move(configuration_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)),
      network_info_(std::move(network_info)),
      build_number_(build_number),
      sensor_capabilities_(sensor_capabilities),
      sd_fs_service_(std::move(sd_fs_service))
#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
      , assets_manager_(std::move(assets_manager))
#endif
{}

int ManagementController::Initialize() {
    config_mgmt_group_ = std::make_unique<ConfigMgmtGroup>(configuration_service_, config_work_queue_thread_);
    if(!config_mgmt_group_->Initialize()) {
        LOG_ERR("Failed to initialize the config SMP group.");
        return -1;
    }

    device_mgmt_group_ = std::make_unique<DeviceMgmtGroup>(build_number_, network_info_, sensor_capabilities_);

    // Served without an SD service too, so the unit says it has no card. The unit works without
    // the group, so a failure leaves only the group out.
    files_mgmt_group_ = std::make_unique<FilesMgmtGroup>(sd_fs_service_, config_work_queue_thread_);
    if(!files_mgmt_group_->Initialize()) {
        LOG_ERR("Failed to initialize the files SMP group.");
        files_mgmt_group_.reset();
    }

#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
    // Served without a registry too, so the unit says it has no kinds. The unit works without the
    // group, so a failure leaves only the group out.
    if(assets_manager_ == nullptr)
        assets_manager_ = std::make_shared<AssetsManager>();

    assets_mgmt_group_ = std::make_unique<AssetsMgmtGroup>(assets_manager_, config_work_queue_thread_);
    if(!assets_mgmt_group_->Initialize()) {
        LOG_ERR("Failed to initialize the assets SMP group.");
        assets_mgmt_group_.reset();
    }
#endif

    config_mgmt_group_->Register();
    device_mgmt_group_->Register();
    if(files_mgmt_group_ != nullptr)
        files_mgmt_group_->Register();
#ifdef CONFIG_EERIE_LEAP_SMP_ASSETS
    if(assets_mgmt_group_ != nullptr)
        assets_mgmt_group_->Register();
#endif

    return 0;
}

} // namespace eerie_leap::controllers
