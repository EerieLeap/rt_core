#pragma once

#include <memory>
#include <zephyr/kernel.h>

#include "subsys/canbus/canbus_proxy.hpp"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/canbus_domain/utilities/can_frame_builder.h"
#include "domain/canbus_domain/processors/i_can_frame_processor.h"

namespace eerie_leap::domain::canbus_domain::services {

using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::domain::canbus_domain::models::CanMessageConfiguration;
using eerie_leap::domain::canbus_domain::utilities::CanFrameBuilder;
using eerie_leap::domain::canbus_domain::processors::ICanFrameProcessor;

struct CanbusTask {
    k_timeout_t send_interval_ms;
    uint8_t bus_channel;
    std::shared_ptr<CanMessageConfiguration> message_configuration;
    std::shared_ptr<CanbusProxy> canbus;
    std::shared_ptr<CanFrameBuilder> can_frame_builder;
    std::shared_ptr<std::vector<std::shared_ptr<ICanFrameProcessor>>> can_frame_processors;
};

} // namespace eerie_leap::domain::canbus_domain::services
