#pragma once

#include <memory>
#include <memory_resource>
#include <string_view>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/json/configs/json_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/services/json_configuration_service.h"

#include "subsys/fs/services/i_fs_service.h"

#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/configuration_domain/utilities/i_json_configuration_manager.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_cbor_parser.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_json_parser.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::configuration::json::configs::JsonCanbusConfig;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::domain::configuration_domain::utilities::IJsonConfigurationManager;
using eerie_leap::domain::canbus_domain::configuration::parsers::CanbusConfigurationCborParser;
using eerie_leap::domain::canbus_domain::configuration::parsers::CanbusConfigurationJsonParser;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::subsys::fs::services::IFsService;

class CanbusConfigurationManager : public IConfigurationManager, public IJsonConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborCanbusConfig>> cbor_configuration_service_;
    std::unique_ptr<config_service::JsonConfigurationService<JsonCanbusConfig>> json_configuration_service_;
    std::shared_ptr<IFsService> sd_fs_service_;

    std::unique_ptr<CanbusConfigurationCborParser> cbor_parser_;
    std::unique_ptr<CanbusConfigurationJsonParser> json_parser_;

    std::shared_ptr<CanbusConfiguration> configuration_;
    uint32_t json_config_checksum_;

    ConfigurationUpdatedHandler configuration_updated_handler_;

    bool ApplyJsonConfiguration(bool fs_load, std::string_view json_str = {});
    bool CreateDefaultConfiguration();

public:
    explicit CanbusConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborCanbusConfig>> cbor_configuration_service,
        std::unique_ptr<config_service::JsonConfigurationService<JsonCanbusConfig>> json_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service);

    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    bool Update(const CanbusConfiguration& configuration);
    std::shared_ptr<CanbusConfiguration> Get(bool force_load = false);

    bool ApplyJsonConfiguration(std::string_view json_str) override;
    std::pmr::string GetJsonConfiguration() override;
};

} // namespace eerie_leap::domain::canbus_domain::configuration
