#include <string>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/device_tree/dt_canbus.h"

#include "domain/canbus_domain/models/canbus_configuration.h"

#include "canbus_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::canbus_domain::models;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(canbus_controller_logger);

CanbusController::CanbusController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<IFsService> sd_fs_service)
    : fs_service_(std::move(fs_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)),
      configuration_service_(std::move(configuration_service)),
      sd_fs_service_(std::move(sd_fs_service)) {}

int CanbusController::Initialize(const ConfigurationSetup& setup_test_configuration) {
    auto cbor_canbus_config_service = std::make_unique<config_services::CborConfigurationService<CborCanbusConfig>>(
        CANBUS_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_);
    canbus_configuration_manager_ = std::make_shared<CanbusConfigurationManager>(
        std::move(cbor_canbus_config_service), sd_fs_service_);

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Canbus, canbus_configuration_manager_);

    // TODO: For test purposes only
    if(setup_test_configuration)
        setup_test_configuration(canbus_configuration_manager_);

    canbus_service_ = std::make_shared<CanbusService>(DtCanbus::Get, canbus_configuration_manager_);

    canbus_com_service_ = std::make_shared<CanbusComService>(canbus_service_);
    if(!canbus_com_service_->Initialize()) {
        LOG_ERR("Failed to initialize the CANBus COM service.");
        return -1;
    }

    network_mgmt_group_ = std::make_unique<NetworkMgmtGroup>(canbus_com_service_->GetNetworkInfo());
    network_mgmt_group_->Register();

    // Registered last so the test configuration above does not trigger a reconfiguration.
    canbus_configuration_manager_->RegisterConfigurationUpdatedHandler([this] { Reconfigure(); });

    return 0;
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
    for(auto& service : dependent_services_)
        service->Stop();

    canbus_service_->Configure();

    canbus_com_service_->Start();
    for(auto& service : dependent_services_)
        service->Start();
}

} // namespace eerie_leap::controllers
