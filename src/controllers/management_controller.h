#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"
#include "subsys/fs/services/i_fs_service.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/smp/config_mgmt_group.h"
#include "domain/system_domain/models/sensor_capabilities.h"
#include "domain/system_domain/smp/device_mgmt_group.h"
#include "domain/system_domain/smp/files_mgmt_group.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::subsys::fs::services::IFsService;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::smp::ConfigMgmtGroup;
using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::domain::system_domain::smp::DeviceMgmtGroup;
using eerie_leap::domain::system_domain::smp::FilesMgmtGroup;

/** @brief Serves the SMP groups of every unit: `config`, `device` and `files`. */
class ManagementController {
private:
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<const ICdmpNetworkInfo> network_info_;
    uint32_t build_number_;
    std::optional<SensorCapabilities> sensor_capabilities_;
    std::shared_ptr<const IFsService> sd_fs_service_;

    std::unique_ptr<ConfigMgmtGroup> config_mgmt_group_;
    std::unique_ptr<DeviceMgmtGroup> device_mgmt_group_;
    std::unique_ptr<FilesMgmtGroup> files_mgmt_group_;

public:
    /**
     * @param configuration_service Must have every configuration manager registered already.
     * @param network_info This unit's CDMP identity; nullptr without CDMP.
     * @param sensor_capabilities What the device INFO reports of the sensor inputs; nothing without sensors.
     * @param sd_fs_service The SD card the `files` group lists; nullptr on a unit without one, which
     *        then answers NO_CARD.
     */
    ManagementController(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<const ICdmpNetworkInfo> network_info,
        uint32_t build_number,
        std::optional<SensorCapabilities> sensor_capabilities = std::nullopt,
        std::shared_ptr<const IFsService> sd_fs_service = nullptr);

    int Initialize();
};

} // namespace eerie_leap::controllers
