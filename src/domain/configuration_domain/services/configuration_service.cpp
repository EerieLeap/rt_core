#include "configuration_service.h"

namespace eerie_leap::domain::configuration_domain::services {

bool ConfigurationService::ApplyCborConfiguration(Type type, std::span<const uint8_t> cbor_data) {
    if(!cbor_configuration_managers_.contains(type))
        return false;

    return cbor_configuration_managers_.at(type)->ApplyCborConfiguration(cbor_data);
}

std::pmr::vector<uint8_t> ConfigurationService::GetCborConfiguration(Type type) {
    if(!cbor_configuration_managers_.contains(type))
        return {};

    return cbor_configuration_managers_.at(type)->GetCborConfiguration();
}

void ConfigurationService::RegisterCborConfigurationManager(
    Type type,
    std::shared_ptr<ICborConfigurationManager> cbor_configuration_manager) {

    cbor_configuration_managers_.try_emplace(type, std::move(cbor_configuration_manager));
}

} // namespace eerie_leap::domain::configuration_domain::services
