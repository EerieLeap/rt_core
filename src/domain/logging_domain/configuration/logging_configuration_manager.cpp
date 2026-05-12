#include <zephyr/logging/log.h>

#include "configuration/json/json_serializer.h"

#include "logging_configuration_manager.h"

namespace eerie_leap::domain::logging_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::json;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(logging_config_ctrl_logger);

LoggingConfigurationManager::LoggingConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborLoggingConfig>> cbor_configuration_service,
    std::unique_ptr<JsonConfigurationService<JsonLoggingConfig>> json_configuration_service)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        json_configuration_service_(std::move(json_configuration_service)),
        configuration_(nullptr) {

    cbor_parser_ = std::make_unique<LoggingConfigurationCborParser>();
    json_parser_ = std::make_unique<LoggingConfigurationJsonParser>();
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

    ApplyJsonConfiguration(true);
}

bool LoggingConfigurationManager::ApplyJsonConfiguration(bool fs_load, std::string_view json_str) {
    if(fs_load && !json_configuration_service_->IsAvailable())
        return false;

    auto json_config_loaded = fs_load
        ? json_configuration_service_->Load()
        : json_configuration_service_->Load(json_str);
    if(!json_config_loaded.has_value())
        return false;

    if(json_config_loaded->checksum == json_config_checksum_)
        return true;

    try {
        auto configuration = json_parser_->Deserialize(Mrm::GetExtPmr(), *json_config_loaded->config);

        json_config_checksum_ = json_config_loaded->checksum;

        if(!Update(*configuration, true))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize JSON configuration. %s", e.what());
        return false;
    }

    LOG_INF("JSON configuration loaded successfully.");

    return true;
}

bool LoggingConfigurationManager::ApplyJsonConfiguration(std::string_view json_str) {
    return ApplyJsonConfiguration(false, json_str);
}

std::pmr::string LoggingConfigurationManager::GetJsonConfiguration() {
    auto json_config = json_parser_->Serialize(*configuration_);
    return JsonSerializer<JsonLoggingConfig>::Serialize(*json_config);
}

bool LoggingConfigurationManager::Update(const LoggingConfiguration& configuration, bool internal_only) {
    try {
        if(!internal_only && json_configuration_service_->IsAvailable()) {
            auto json_config = json_parser_->Serialize(configuration);
            json_configuration_service_->Save(json_config.get());

            auto json_config_loaded = json_configuration_service_->Load();
            if(!json_config_loaded.has_value()) {
                LOG_ERR("Failed to load newly updated JSON configuration.");
                return false;
            }

            LOG_INF("JSON configuration updated successfully.");

            json_config_checksum_ = json_config_loaded->checksum;
        }

        auto cbor_config = cbor_parser_->Serialize(configuration);
        cbor_config->json_config_checksum = json_config_checksum_;

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

    json_config_checksum_ = cbor_config->json_config_checksum;

    return configuration_;
}

bool LoggingConfigurationManager::CreateDefaultConfiguration() {
    auto configuration = make_unique_pmr<LoggingConfiguration>(Mrm::GetExtPmr());

    return Update(*configuration);
}

} // namespace eerie_leap::domain::logging_domain::configuration
