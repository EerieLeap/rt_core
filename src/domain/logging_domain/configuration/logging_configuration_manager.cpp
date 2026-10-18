#include "subsys/threading/scoped_mutex.h"

#include "logging_configuration_manager.h"

namespace eerie_leap::domain::logging_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using eerie_leap::subsys::threading::ScopedMutex;

LoggingConfigurationManager::LoggingConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborLoggingConfig>> cbor_configuration_service
) :
    CborConfigurationManagerBase("Logging", std::move(cbor_configuration_service)) {

    // Before the default is created: that store notifies too.
    k_mutex_init(&handler_mutex_);

    LoadOrCreateDefault();
}

void LoggingConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    std::shared_ptr<const ConfigurationUpdatedHandler> shared_handler;
    if(handler)
        shared_handler = std::make_shared<const ConfigurationUpdatedHandler>(std::move(handler));

    {
        ScopedMutex guard(handler_mutex_);
        configuration_updated_handler_.swap(shared_handler);
    }

    // The previous handler is released here, outside the lock, unless a store still calls it.
}

void LoggingConfigurationManager::NotifyUpdated() {
    std::shared_ptr<const ConfigurationUpdatedHandler> handler;
    {
        ScopedMutex guard(handler_mutex_);
        handler = configuration_updated_handler_;
    }

    if(handler != nullptr)
        (*handler)();
}

bool LoggingConfigurationManager::Update(const LoggingConfiguration& configuration) {
    if(!CborConfigurationManagerBase::Update(configuration))
        return false;

    NotifyUpdated();

    return true;
}

bool LoggingConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    NotifyUpdated();

    return true;
}

PmrUniquePtr<CborLoggingConfig> LoggingConfigurationManager::Serialize(const LoggingConfiguration& configuration) {
    return LoggingConfigurationCborParser::Serialize(configuration);
}

PmrUniquePtr<LoggingConfiguration> LoggingConfigurationManager::Deserialize(const CborLoggingConfig& cbor_config) {
    return LoggingConfigurationCborParser::Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool LoggingConfigurationManager::CreateDefaultConfiguration() {
    return Update(*make_unique_pmr<LoggingConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::logging_domain::configuration
