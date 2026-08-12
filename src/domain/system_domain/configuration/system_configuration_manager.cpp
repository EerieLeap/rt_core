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
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        configuration_(nullptr) {

    cbor_parser_ = std::make_unique<SystemConfigurationCborParser>();
    std::shared_ptr<SystemConfiguration> configuration = nullptr;

    try {
        configuration = Get(true);
    } catch(...) {
        LOG_ERR("Failed to load System configuration.");
    }

    if(configuration == nullptr) {
        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default System configuration.");
            return;
        }

        LOG_INF("Default System configuration loaded successfully.");

        configuration = Get();
    }

    LOG_INF("System Configuration Manager initialized successfully.");

    LOG_INF("HW Version: %s, SW Version: %s",
        configuration_->GetFormattedHwVersion().c_str(), configuration_->GetFormattedSwVersion().c_str());
    LOG_INF("Device ID: %llu", configuration_->device_id);
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

bool SystemConfigurationManager::Update(const SystemConfiguration& configuration) {
    try {
        auto cbor_config = cbor_parser_->Serialize(configuration);

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update System configuration. %s", e.what());
        return false;
    }

    return Get(true) != nullptr;
}

std::shared_ptr<SystemConfiguration> SystemConfigurationManager::Get(bool force_load) {
    if(configuration_ != nullptr && !force_load)
        return configuration_;

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    auto configuration = cbor_parser_->Deserialize(Mrm::GetDefaultPmr(), *cbor_config);
    configuration_ = std::make_shared<SystemConfiguration>(std::move(*configuration));

    return configuration_;
}

bool SystemConfigurationManager::CreateDefaultConfiguration() {
    auto configuration = make_unique_pmr<SystemConfiguration>(Mrm::GetDefaultPmr());

    configuration->device_id = Rng::Get<uint64_t>(true);
    configuration->build_number = 0;

    return Update(*configuration);
}

} // namespace eerie_leap::domain::system_domain::configuration
