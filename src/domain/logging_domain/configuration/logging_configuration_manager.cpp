#include <zephyr/logging/log.h>

#include "logging_configuration_manager.h"

namespace eerie_leap::domain::logging_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(logging_config_ctrl_logger);

LoggingConfigurationManager::LoggingConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborLoggingConfig>> cbor_configuration_service)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        configuration_(nullptr) {

    cbor_parser_ = std::make_unique<LoggingConfigurationCborParser>();
    std::shared_ptr<LoggingConfiguration> configuration = nullptr;

    try {
        configuration = Get(true);
    } catch(...) {
        LOG_ERR("Failed to load Logging configuration.");
    }

    if(configuration == nullptr) {
        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default Logging configuration.");
            return;
        }

        LOG_INF("Default Logging configuration loaded successfully.");
    }

    LOG_INF("Logging Configuration Manager initialized successfully.");
}

bool LoggingConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
    if(cbor_config == nullptr)
        return false;

    try {
        auto configuration = cbor_parser_->Deserialize(Mrm::GetExtPmr(), *cbor_config);

        if(!Update(*configuration))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize CBOR configuration. %s", e.what());
        return false;
    }

    LOG_INF("CBOR configuration loaded successfully.");

    return true;
}

std::pmr::vector<uint8_t> LoggingConfigurationManager::GetCborConfiguration() {
    auto cbor_config = cbor_parser_->Serialize(*configuration_);

    return cbor_configuration_service_->Serialize(*cbor_config);
}

bool LoggingConfigurationManager::Update(const LoggingConfiguration& configuration) {
    try {
        auto cbor_config = cbor_parser_->Serialize(configuration);

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update Logging configuration. %s", e.what());
        return false;
    }

    return Get(true) != nullptr;
}

std::shared_ptr<LoggingConfiguration> LoggingConfigurationManager::Get(bool force_load) {
    if (configuration_ != nullptr && !force_load) {
        return configuration_;
    }

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    auto configuration = cbor_parser_->Deserialize(Mrm::GetExtPmr(), *cbor_config);
    configuration_ = make_shared_pmr<LoggingConfiguration>(Mrm::GetExtPmr(), std::move(*configuration));

    return configuration_;
}

bool LoggingConfigurationManager::CreateDefaultConfiguration() {
    auto configuration = make_unique_pmr<LoggingConfiguration>(Mrm::GetExtPmr());

    return Update(*configuration);
}

} // namespace eerie_leap::domain::logging_domain::configuration
