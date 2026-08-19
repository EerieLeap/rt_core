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

bool ConfigurationService::ApplyJsonConfiguration(Type type, std::string_view json_str) {
    if(!json_configuration_managers_.contains(type))
        return false;

    return json_configuration_managers_.at(type)->ApplyJsonConfiguration(json_str);
}

std::pmr::string ConfigurationService::GetJsonConfiguration(Type type) {
    if(!json_configuration_managers_.contains(type))
        return "";

    return json_configuration_managers_.at(type)->GetJsonConfiguration();
}

void ConfigurationService::RegisterJsonConfigurationManager(
    Type type,
    std::shared_ptr<IJsonConfigurationManager> json_configuration_manager) {

    json_configuration_managers_.try_emplace(type, std::move(json_configuration_manager));
}

} // namespace eerie_leap::domain::configuration_domain::services
