#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"

#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/smp/network_mgmt_group.h"

namespace eerie_leap::subsys::assets {
class AssetsManager;
}

namespace eerie_leap::controllers {

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::smp::NetworkMgmtGroup;

class CanbusController {
public:
    using ConfigurationSetup = std::function<void(std::shared_ptr<CanbusConfigurationManager>)>;

private:
    static constexpr const char* CANBUS_CONFIGURATION_NAME = "canbus_config";

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<IFsService> sd_fs_service_;
    std::shared_ptr<AssetsManager> assets_manager_;

    std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager_;
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<CanbusComService> canbus_com_service_;
    std::unique_ptr<NetworkMgmtGroup> network_mgmt_group_;

    std::vector<std::shared_ptr<IService>> dependent_services_;

    std::optional<WorkQueueTask<CanbusController>> reconfigure_task_;
    // Whether this controller registered the CAN scripts with assets_manager_, and so unregisters them.
    bool is_assets_registered_ = false;

    void Reconfigure();
    // Serves the CAN scripts as an asset kind. With CONFIG_EERIE_LEAP_ASSETS, a manager and an SD
    // service only; registers nothing otherwise.
    void RegisterAssets();
    void UnregisterAssets();

public:
    CanbusController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        // The scripts directory of this volume is served as the CAN_SCRIPTS asset kind.
        std::shared_ptr<IFsService> sd_fs_service = nullptr,
        // With CONFIG_EERIE_LEAP_ASSETS: the registry the CAN scripts are registered with, once
        // Initialize() succeeded, when there is an SD service too; ignored otherwise.
        std::shared_ptr<AssetsManager> assets_manager = nullptr);
    ~CanbusController();

    int Initialize(const ConfigurationSetup& setup_test_configuration = nullptr);
    int Start();

    void RegisterDependentService(std::shared_ptr<IService> service);

    std::shared_ptr<CanbusConfigurationManager> GetConfigurationManager() const { return canbus_configuration_manager_; }
    std::shared_ptr<CanbusService> GetService() const { return canbus_service_; }
    std::shared_ptr<CanbusComService> GetComService() const { return canbus_com_service_; }
};

} // namespace eerie_leap::controllers
