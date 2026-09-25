#include "eerie_memory.hpp"

#include "subsys/random/rng.h"

#include "system_configuration_manager.h"

namespace eerie_leap::domain::system_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::subsys::random;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(system_config_mngr_logger);

SystemConfigurationManager::SystemConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborSystemConfig>> cbor_configuration_service)
        : CborConfigurationManagerBase("System", std::move(cbor_configuration_service)) {

    if(!LoadOrCreateDefault())
        return;

    auto configuration = Get();

    LOG_INF("HW Version: %s, SW Version: %s",
        configuration->GetFormattedHwVersion().c_str(), configuration->GetFormattedSwVersion().c_str());
    LOG_INF("Device ID: %llu", configuration->device_id);
}

bool SystemConfigurationManager::UpdateBuildNumber(uint32_t build_number) {
    auto configuration = Get();
    if(configuration == nullptr)
        return false;

    if(build_number != configuration->build_number) {
        configuration->build_number = build_number;

        if(!Update(*configuration))
            return false;

        LOG_INF("Build number updated to %u.", configuration->build_number);
    }

    return true;
}

pmr_unique_ptr<CborSystemConfig> SystemConfigurationManager::Serialize(const SystemConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<SystemConfiguration> SystemConfigurationManager::Deserialize(const CborSystemConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetDefaultPmr(), cbor_config);
}

bool SystemConfigurationManager::CreateDefaultConfiguration() {
    auto configuration = make_unique_pmr<SystemConfiguration>(Mrm::GetDefaultPmr());

    configuration->device_id = Rng::Get<uint64_t>(true);
    configuration->build_number = 0;

    return Update(*configuration);
}

} // namespace eerie_leap::domain::system_domain::configuration
