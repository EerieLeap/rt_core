#include "can_signal_codec.h"

#include "can_message_codec.h"

namespace eerie_leap::domain::canbus_domain::utilities {

std::vector<uint8_t> CanMessageCodec::Encode(
    const CanMessageConfiguration& message_configuration,
    const SignalValueReader& signal_value_reader) {

    std::vector<uint8_t> data(message_configuration.message_size, 0);

    for(const auto& signal_configuration : message_configuration.signal_configurations)
        CanSignalCodec::Encode(signal_configuration, signal_value_reader(signal_configuration), data);

    return data;
}

} // namespace eerie_leap::domain::canbus_domain::utilities
