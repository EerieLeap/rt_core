#include "configuration_service.h"

namespace eerie_leap::domain::configuration_domain::services {

bool ConfigurationService::ApplyJsonConfiguration(Type type, std::span<const uint8_t> data) {
    if(!json_configuration_managers_.contains(type))
        return false;

    return json_configuration_managers_.at(type)->ApplyJsonConfiguration(data);
}

void ConfigurationService::RegisterJsonConfigurationManager(
    Type type,
    std::shared_ptr<IJsonConfigurationManager> json_configuration_manager) {

    json_configuration_managers_.insert({ type, std::move(json_configuration_manager) });
}

} // namespace eerie_leap::domain::configuration_domain::services
