#pragma once

#include <memory>
#include <unordered_map>
#include <streambuf>
#include <functional>

#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"

namespace eerie_leap::domain::canbus_domain::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::canbus_domain::configuration;

using eerie_leap::subsys::canbus::CanbusProxy;

class CanbusService {
public:
    using CanbusServiceUpdatedHandler = std::function<void()>;

private:
    std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager_;
    std::function<const device*(uint8_t)> dt_canbus_provider_;

    std::unordered_map<uint8_t, std::shared_ptr<Canbus>> canbuses_;
    std::unordered_map<uint8_t, std::shared_ptr<CanbusProxy>> canbus_proxies_;

    int configuration_reset_next_handler_id_ = 1;
    std::unordered_map<int, CanbusServiceUpdatedHandler> configuration_reset_handlers_;
    int configuration_updated_next_handler_id_ = 1;
    std::unordered_map<int, CanbusServiceUpdatedHandler> configuration_updated_handlers_;

    void BitrateUpdated(uint8_t bus_channel, uint32_t bitrate) const;
    void ConfigureUserSignals(const CanChannelConfiguration& channel_configuration) const;

public:
    CanbusService(
        std::function<const device*(uint8_t)> dt_canbus_provider,
        std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager);
    void Configure();

    [[nodiscard]] std::shared_ptr<CanbusProxy> GetCanbus(uint8_t bus_channel) const;
    [[nodiscard]] std::shared_ptr<CanbusProxy> GetComCanbus() const;

    [[nodiscard]] const CanChannelConfiguration* GetChannelConfiguration(uint8_t bus_channel) const;

    int RegisterConfigurationResetHandler(CanbusServiceUpdatedHandler handler);
    bool UnregisterConfigurationResetHandler(int handler_id);
    int RegisterConfigurationUpdatedHandler(CanbusServiceUpdatedHandler handler);
    bool UnregisterConfigurationUpdatedHandler(int handler_id);
};

} // namespace eerie_leap::domain::canbus_domain::services
