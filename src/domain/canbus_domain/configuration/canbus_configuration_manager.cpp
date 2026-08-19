#include <zephyr/sys/crc.h>
#include <eerie_memory.hpp>

#include "configuration/json/json_serializer.h"

#include "canbus_configuration_manager.h"

namespace eerie_leap::domain::canbus_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::json;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(canbus_config_mngr_logger);

CanbusConfigurationManager::CanbusConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborCanbusConfig>> cbor_configuration_service,
    std::unique_ptr<JsonConfigurationService<JsonCanbusConfig>> json_configuration_service,
    std::shared_ptr<IFsService> sd_fs_service)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        json_configuration_service_(std::move(json_configuration_service)),
        sd_fs_service_(std::move(sd_fs_service)),
        configuration_(nullptr),
        json_config_checksum_(0) {

    cbor_parser_ = std::make_unique<CanbusConfigurationCborParser>(sd_fs_service_);
    json_parser_ = std::make_unique<CanbusConfigurationJsonParser>(sd_fs_service_);
    std::shared_ptr<CanbusConfiguration> configuration = nullptr;

    try {
        configuration = Get(true);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to load CAN Bus configuration: %s", e.what());
    }

    if(configuration == nullptr) {
        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default CAN Bus configuration.");
            return;
        }

        LOG_INF("Default CAN Bus configuration loaded successfully.");
    }

    LOG_INF("CAN Bus Configuration Manager initialized successfully.");

    ApplyJsonConfiguration(true);
}

void CanbusConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = handler;
}

bool CanbusConfigurationManager::ApplyJsonConfiguration(bool fs_load, std::string_view json_str) {
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

        if(!Update(*configuration))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize JSON configuration. %s", e.what());
        return false;
    }

    LOG_INF("JSON configuration loaded successfully.");

    return true;
}

bool CanbusConfigurationManager::ApplyJsonConfiguration(std::string_view json_str) {
    return ApplyJsonConfiguration(false, json_str);
}

std::pmr::string CanbusConfigurationManager::GetJsonConfiguration() {
    auto configuration = Get();

    auto json_config = json_parser_->Serialize(*configuration);
    return JsonSerializer<JsonCanbusConfig>::Serialize(*json_config);
}

bool CanbusConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
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

std::pmr::vector<uint8_t> CanbusConfigurationManager::GetCborConfiguration() {
    auto configuration = Get();

    auto cbor_config = cbor_parser_->Serialize(*configuration);
    cbor_config->json_config_checksum = json_config_checksum_;

    return cbor_configuration_service_->Serialize(*cbor_config);
}

bool CanbusConfigurationManager::Update(const CanbusConfiguration& configuration) {
    try {
        if(json_configuration_service_->IsAvailable()) {
            auto json_config = json_parser_->Serialize(configuration);
            json_configuration_service_->Save(json_config.get());

            auto json_config_loaded = json_configuration_service_->Load();
            if(!json_config_loaded.has_value()) {
                LOG_ERR("Failed to load newly updated CAN Bus JSON configuration.");
                return false;
            }

            LOG_INF("CAN Bus JSON configuration updated successfully.");

            json_config_checksum_ = json_config_loaded->checksum;
        }

        auto cbor_config = cbor_parser_->Serialize(configuration);
        cbor_config->json_config_checksum = json_config_checksum_;

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update CAN Bus configuration. %s", e.what());
        return false;
    }

    bool result = Get(true) != nullptr;

    if(result && configuration_updated_handler_)
        configuration_updated_handler_();

    return result;
}

std::shared_ptr<CanbusConfiguration> CanbusConfigurationManager::Get(bool force_load) {
    if(configuration_ != nullptr && !force_load)
        return configuration_;

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    auto configuration = cbor_parser_->Deserialize(Mrm::GetExtPmr(), *cbor_config);
    configuration_ = make_shared_pmr<CanbusConfiguration>(Mrm::GetExtPmr(), std::move(*configuration));

    json_config_checksum_ = cbor_config->json_config_checksum;

    return configuration_;
}

bool CanbusConfigurationManager::CreateDefaultConfiguration() {
    auto configuration = make_unique_pmr<CanbusConfiguration>(Mrm::GetExtPmr());

    return Update(*configuration);
}

} // namespace eerie_leap::domain::canbus_domain::configuration
