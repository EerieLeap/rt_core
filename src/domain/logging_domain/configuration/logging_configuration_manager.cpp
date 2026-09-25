#include "logging_configuration_manager.h"

namespace eerie_leap::domain::logging_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;

LoggingConfigurationManager::LoggingConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborLoggingConfig>> cbor_configuration_service)
        : CborConfigurationManagerBase("Logging", std::move(cbor_configuration_service)) {

    LoadOrCreateDefault();
}

pmr_unique_ptr<CborLoggingConfig> LoggingConfigurationManager::Serialize(const LoggingConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<LoggingConfiguration> LoggingConfigurationManager::Deserialize(const CborLoggingConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool LoggingConfigurationManager::CreateDefaultConfiguration() {
    return Update(*make_unique_pmr<LoggingConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::logging_domain::configuration
