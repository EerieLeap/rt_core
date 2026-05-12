#pragma once

#include <zephyr/data/json.h>
#include <eerie_memory.hpp>

#include "configuration/json/configs/json_canbus_config.h"
#include "subsys/fs/services/i_fs_service.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration::parsers {

using eerie_leap::configuration::json::configs::JsonCanbusConfig;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;

class CanbusConfigurationJsonParser {
private:
    std::shared_ptr<IFsService> sd_fs_service_;

public:
    explicit CanbusConfigurationJsonParser(std::shared_ptr<IFsService> sd_fs_service);

    eerie_memory::pmr_unique_ptr<JsonCanbusConfig> Serialize(const CanbusConfiguration& configuration);
    eerie_memory::pmr_unique_ptr<CanbusConfiguration> Deserialize(std::pmr::memory_resource* mr, const JsonCanbusConfig& config);
};

} // namespace eerie_leap::domain::canbus_domain::configuration::parsers
