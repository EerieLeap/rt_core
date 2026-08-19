#pragma once

#include <memory>
#include <unordered_map>
#include <streambuf>
#include <functional>

#include "../utilities/i_cbor_configuration_manager.h"

namespace eerie_leap::domain::configuration_domain::services {

using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;

class ConfigurationService {
public:
    enum class Type : uint8_t {
        System = 0x01,
        Adc = 0x02,
        Canbus = 0x03,
        Sensors = 0x04,
        Logging = 0x05,
        Ui = 0x06,
    };

private:
    std::unordered_map<Type, std::shared_ptr<ICborConfigurationManager>> cbor_configuration_managers_;

public:
    ConfigurationService() = default;
    virtual ~ConfigurationService() = default;

    bool ApplyCborConfiguration(Type type, std::span<const uint8_t> cbor_data);
    std::pmr::vector<uint8_t> GetCborConfiguration(Type type);
    void RegisterCborConfigurationManager(
        Type type,
        std::shared_ptr<ICborConfigurationManager> cbor_configuration_manager);
};

} // namespace eerie_leap::domain::configuration_domain::services
