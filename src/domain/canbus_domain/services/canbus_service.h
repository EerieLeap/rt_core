#pragma once

#include <memory>
#include <unordered_map>
#include <streambuf>
#include <functional>

#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/canbus/canbus.h"
#include "subsys/dbc/dbc.h"
#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"

namespace eerie_leap::domain::canbus_domain::services {

using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::dbc;
using namespace eerie_leap::domain::canbus_domain::configuration;

class CanbusService {
private:
    std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager_;
    std::function<const device*(uint8_t)> dt_canbus_provider_;

    std::unordered_map<uint8_t, std::shared_ptr<Canbus>> canbuses_;

    void BitrateUpdated(uint8_t bus_channel, uint32_t bitrate) const;
    void ConfigureUserSignals(const CanChannelConfiguration& channel_configuration) const;

public:
    CanbusService(
        std::function<const device*(uint8_t)> dt_canbus_provider,
        std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager);
    void Configure();

    [[nodiscard]] std::shared_ptr<Canbus> GetCanbus(uint8_t bus_channel) const;
    [[nodiscard]] std::shared_ptr<Canbus> GetComCanbus() const;
    [[nodiscard]] const CanChannelConfiguration* GetChannelConfiguration(uint8_t bus_channel) const;
};

} // namespace eerie_leap::domain::canbus_domain::services
