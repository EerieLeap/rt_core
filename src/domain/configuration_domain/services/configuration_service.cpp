#include <algorithm>

#include "configuration_service.h"

namespace eerie_leap::domain::configuration_domain::services {

bool ConfigurationService::ApplyCborConfiguration(Type type, std::span<const uint8_t> cbor_data) {
    if(!cbor_configuration_managers_.contains(type))
        return false;

    return cbor_configuration_managers_.at(type).manager->ApplyCborConfiguration(cbor_data);
}

std::pmr::vector<uint8_t> ConfigurationService::GetCborConfiguration(Type type) {
    if(!cbor_configuration_managers_.contains(type))
        return {};

    return cbor_configuration_managers_.at(type).manager->GetCborConfiguration();
}

StoredCborInfo ConfigurationService::GetCborConfigurationInfo(Type type) const {
    auto it = cbor_configuration_managers_.find(type);

    return it != cbor_configuration_managers_.end() ? it->second.manager->GetCborConfigurationInfo() : StoredCborInfo{};
}

void ConfigurationService::RegisterCborConfigurationManager(
    Type type,
    std::shared_ptr<ICborConfigurationManager> cbor_configuration_manager,
    size_t max_cbor_size) {

    cbor_configuration_managers_.try_emplace(
        type, Registration{.manager = std::move(cbor_configuration_manager), .max_cbor_size = max_cbor_size});
}

bool ConfigurationService::IsRegistered(Type type) const {
    return cbor_configuration_managers_.contains(type);
}

size_t ConfigurationService::GetMaxCborSize(Type type) const {
    auto it = cbor_configuration_managers_.find(type);

    return it != cbor_configuration_managers_.end() ? it->second.max_cbor_size : 0;
}

size_t ConfigurationService::GetRegisteredTypes(std::span<Type> types) const {
    size_t count = 0;
    for(const auto& [type, _] : cbor_configuration_managers_) {
        if(count == types.size())
            break;

        types[count++] = type;
    }

    std::sort(types.begin(), types.begin() + count);

    return count;
}

} // namespace eerie_leap::domain::configuration_domain::services
