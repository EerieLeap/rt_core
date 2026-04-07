#pragma once

#include <memory>

#include "utilities/memory/heap_allocator.h"
#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/services/json_configuration_service.h"
#include "domain/system_domain/configuration/parsers/system_configuration_cbor_parser.h"
#include "domain/system_domain/models/system_configuration.h"

namespace eerie_leap::domain::system_domain::configuration {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::domain::system_domain::configuration::parsers;
using namespace eerie_leap::domain::system_domain::models;

class SystemConfigurationManager {
private:
    std::unique_ptr<CborConfigurationService<CborSystemConfig>> cbor_configuration_service_;

    std::unique_ptr<SystemConfigurationCborParser> cbor_parser_;

    std::shared_ptr<SystemConfiguration> configuration_;

    bool CreateDefaultConfiguration();

public:
    explicit SystemConfigurationManager(
        std::unique_ptr<CborConfigurationService<CborSystemConfig>> cbor_configuration_service);

    bool UpdateBuildNumber(uint32_t build_number);

    bool Update(const SystemConfiguration& configuration);
    std::shared_ptr<SystemConfiguration> Get(bool force_load = false);
};

} // namespace eerie_leap::domain::system_domain::configuration
