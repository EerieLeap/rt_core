#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/json/configs/json_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/services/json_configuration_service.h"

#include "subsys/fs/services/i_fs_service.h"

#include "domain/configuration_domain/utilities/i_json_configuration_manager.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_cbor_parser.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_json_parser.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::json::configs;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::domain::configuration_domain::utilities;
using namespace eerie_leap::domain::canbus_domain::configuration::parsers;
using namespace eerie_leap::domain::canbus_domain::models;

class CanbusConfigurationManager : public IJsonConfigurationManager {
private:
    std::unique_ptr<CborConfigurationService<CborCanbusConfig>> cbor_configuration_service_;
    std::unique_ptr<JsonConfigurationService<JsonCanbusConfig>> json_configuration_service_;
    std::shared_ptr<IFsService> sd_fs_service_;

    std::unique_ptr<CanbusConfigurationCborParser> cbor_parser_;
    std::unique_ptr<CanbusConfigurationJsonParser> json_parser_;

    std::shared_ptr<CanbusConfiguration> configuration_;

    uint32_t json_config_checksum_;

    bool ApplyJsonConfiguration(bool fs_load, std::string_view json_str = {});
    bool CreateDefaultConfiguration();

public:
    explicit CanbusConfigurationManager(
        std::unique_ptr<CborConfigurationService<CborCanbusConfig>> cbor_configuration_service,
        std::unique_ptr<JsonConfigurationService<JsonCanbusConfig>> json_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service);

    bool Update(const CanbusConfiguration& configuration);
    std::shared_ptr<CanbusConfiguration> Get(bool force_load = false);

    bool ApplyJsonConfiguration(std::string_view json_str) override;
    std::pmr::string GetJsonConfiguration() override;
};

} // namespace eerie_leap::domain::canbus_domain::configuration
