#include <utility>

#include "utilities/memory/memory_resource_manager.h"

#include "system_configuration_validator.h"
#include "system_configuration_cbor_parser.h"

namespace eerie_leap::domain::system_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;

pmr_unique_ptr<CborSystemConfig> SystemConfigurationCborParser::Serialize(const SystemConfiguration& configuration) {
    SystemConfigurationValidator::Validate(configuration);

    auto config = make_unique_pmr<CborSystemConfig>(Mrm::GetExtPmr());
    memset(config.get(), 0, sizeof(CborSystemConfig));

    config->device_id = configuration.device_id;
    config->build_number = configuration.build_number;

    return config;
}

pmr_unique_ptr<SystemConfiguration> SystemConfigurationCborParser::Deserialize(
    std::pmr::memory_resource* mr,
    const CborSystemConfig& config) {

    auto configuration = make_unique_pmr<SystemConfiguration>(mr);

    configuration->device_id = config.device_id;
    configuration->build_number = config.build_number;

    SystemConfigurationValidator::Validate(*configuration);

    return configuration;
}

} // namespace eerie_leap::domain::system_domain::configuration::parsers
