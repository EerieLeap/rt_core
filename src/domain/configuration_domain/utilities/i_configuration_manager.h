#pragma once

#include <cstdint>
#include <span>
#include <memory_resource>
#include <functional>

namespace eerie_leap::domain::configuration_domain::utilities {

class IConfigurationManager {
public:
    using ConfigurationUpdatedHandler = std::function<void()>;

    virtual ~IConfigurationManager() = default;

    virtual void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) = 0;
};

} // namespace eerie_leap::domain::configuration_domain::utilities
