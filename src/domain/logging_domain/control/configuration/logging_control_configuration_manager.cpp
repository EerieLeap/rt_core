#include <exception>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "logging_control_configuration_manager.h"

namespace eerie_leap::domain::logging_domain::control::configuration {

LOG_MODULE_REGISTER(logging_control_configuration_manager_logger);

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using eerie_leap::subsys::threading::ScopedMutex;

LoggingControlConfigurationManager::LoggingControlConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborLoggingControlConfig>> cbor_configuration_service
) :
    CborConfigurationManagerBase("LoggingControl", std::move(cbor_configuration_service)) {

    // Before the default is created: that store notifies too.
    k_mutex_init(&handler_mutex_);

    LoadOrCreateDefault();
}

void LoggingControlConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    std::shared_ptr<const ConfigurationUpdatedHandler> shared_handler;
    if(handler)
        shared_handler = std::make_shared<const ConfigurationUpdatedHandler>(std::move(handler));

    {
        ScopedMutex guard(handler_mutex_);
        configuration_updated_handler_.swap(shared_handler);
    }

    // The previous handler is released here, outside the lock, unless a store still calls it.
}

void LoggingControlConfigurationManager::NotifyUpdated() {
    std::shared_ptr<const ConfigurationUpdatedHandler> handler;
    {
        ScopedMutex guard(handler_mutex_);
        handler = configuration_updated_handler_;
    }

    if(handler == nullptr)
        return;

    // The configuration is stored and in use, so a handler that fails (a trigger set that could not
    // be rebuilt) must not turn the apply into a failure the client would retry.
    try {
        (*handler)();
    } catch(const std::exception& e) {
        LOG_ERR("The logging control configuration was stored, but its updated handler failed: %s", e.what());
    } catch(...) {
        LOG_ERR("The logging control configuration was stored, but its updated handler failed.");
    }
}

bool LoggingControlConfigurationManager::Update(const LoggingControlConfiguration& configuration) {
    if(!CborConfigurationManagerBase::Update(configuration))
        return false;

    NotifyUpdated();

    return true;
}

bool LoggingControlConfigurationManager::ApplyCborConfiguration(
    std::span<const uint8_t> cbor_data,
    std::span<char> reason
) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    NotifyUpdated();

    return true;
}

pmr_unique_ptr<CborLoggingControlConfig>
LoggingControlConfigurationManager::Serialize(const LoggingControlConfiguration& configuration) {

    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<LoggingControlConfiguration>
LoggingControlConfigurationManager::Deserialize(const CborLoggingControlConfig& cbor_config) {

    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool LoggingControlConfigurationManager::CreateDefaultConfiguration() {
    return Update(*make_unique_pmr<LoggingControlConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::logging_domain::control::configuration
