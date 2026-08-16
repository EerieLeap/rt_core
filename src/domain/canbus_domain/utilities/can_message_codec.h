#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "domain/canbus_domain/models/can_message_configuration.h"

namespace eerie_leap::domain::canbus_domain::utilities {

using eerie_leap::domain::canbus_domain::models::CanMessageConfiguration;
using eerie_leap::domain::canbus_domain::models::CanSignalConfiguration;

// Assembles a frame payload out of the values of its signals.
class CanMessageCodec {
public:
    using SignalValueReader = std::function<float (const CanSignalConfiguration&)>;

    static std::vector<uint8_t> Encode(const CanMessageConfiguration& message_configuration, const SignalValueReader& signal_value_reader);
};

} // namespace eerie_leap::domain::canbus_domain::utilities
