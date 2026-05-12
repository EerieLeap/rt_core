#pragma once

#include <memory>
#include <unordered_map>
#include <streambuf>
#include <functional>

#include "../utilities/i_json_configuration_manager.h"

namespace eerie_leap::domain::configuration_domain::services {

using eerie_leap::domain::configuration_domain::utilities::IJsonConfigurationManager;

class ConfigurationService {
public:
    enum class Type : uint8_t {
        SystemJson = 0x01,
        AdcJson = 0x02,
        CanbusJson = 0x03,
        SensorsJson = 0x04,
        LoggingJson = 0x05,
        UiJson = 0x06,
    };

private:
    std::unordered_map<Type, std::shared_ptr<IJsonConfigurationManager>> json_configuration_managers_;

public:
    ConfigurationService() = default;
    virtual ~ConfigurationService() = default;

    bool ApplyJsonConfiguration(Type type, std::string_view json_str);
    std::pmr::string GetJsonConfiguration(Type type);
    void RegisterJsonConfigurationManager(
        Type type,
        std::shared_ptr<IJsonConfigurationManager> json_configuration_manager);
};

} // namespace eerie_leap::domain::configuration_domain::services
