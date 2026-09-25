#include <eerie_memory.hpp>

#include "canbus_configuration_manager.h"

namespace eerie_leap::domain::canbus_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;

CanbusConfigurationManager::CanbusConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborCanbusConfig>> cbor_configuration_service,
    std::shared_ptr<IFsService> sd_fs_service)
        : CborConfigurationManagerBase("CAN Bus", std::move(cbor_configuration_service)),
        cbor_parser_(std::move(sd_fs_service)
) {
    LoadOrCreateDefault();
}

void CanbusConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

bool CanbusConfigurationManager::Update(const CanbusConfiguration& configuration) {
    if(!CborConfigurationManagerBase::Update(configuration))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

bool CanbusConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

pmr_unique_ptr<CborCanbusConfig> CanbusConfigurationManager::Serialize(const CanbusConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<CanbusConfiguration> CanbusConfigurationManager::Deserialize(const CborCanbusConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool CanbusConfigurationManager::CreateDefaultConfiguration() {
    return Update(*make_unique_pmr<CanbusConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::canbus_domain::configuration
