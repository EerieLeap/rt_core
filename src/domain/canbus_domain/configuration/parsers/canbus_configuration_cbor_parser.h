#pragma once

#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "subsys/fs/services/i_fs_service.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration::parsers {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;

class CanbusConfigurationCborParser {
private:
    std::shared_ptr<IFsService> sd_fs_service_;

public:
    explicit CanbusConfigurationCborParser(std::shared_ptr<IFsService> sd_fs_service);

    eerie_memory::pmr_unique_ptr<CborCanbusConfig> Serialize(const CanbusConfiguration& configuration);
    eerie_memory::pmr_unique_ptr<CanbusConfiguration> Deserialize(std::pmr::memory_resource* mr, const CborCanbusConfig& config);
};

} // namespace eerie_leap::domain::canbus_domain::configuration::parsers
