#pragma once

#include <cstdint>
#include <span>
#include <memory_resource>

namespace eerie_leap::domain::configuration_domain::utilities {

class IJsonConfigurationManager {
public:
    virtual ~IJsonConfigurationManager() = default;

    virtual bool ApplyJsonConfiguration(std::string_view json_str) = 0;
    virtual std::pmr::string GetJsonConfiguration() = 0;
};

} // namespace eerie_leap::domain::configuration_domain::utilities
