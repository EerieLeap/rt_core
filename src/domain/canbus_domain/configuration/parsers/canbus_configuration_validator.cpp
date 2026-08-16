#include <stdexcept>
#include <string>
#include <unordered_set>
#include <algorithm>

#include "canbus_configuration_validator.h"
#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/canbus_domain/utilities/can_signal_codec.h"
#include "subsys/canbus/canbus.h"

namespace eerie_leap::domain::canbus_domain::configuration::parsers {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::canbus_domain::models;

using eerie_leap::domain::canbus_domain::utilities::CanSignalCodec;

static void InvalidCanbusConfiguration(const uint8_t bus_channel, std::string_view message) {
    throw std::invalid_argument(
        "Invalid CAN Bus configuration. Channel: "
        + std::to_string(bus_channel)
        + ". "
        + std::string(message));
}

static void InvalidCanMessageConfiguration(const uint8_t bus_channel, uint32_t frame_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid CAN Frame configuration. Channel: "
        + std::to_string(bus_channel)
        + ", Frame ID: "
        + std::to_string(frame_id)
        + ". "
        + std::string(message));
}

static void InvalidCanSignalConfiguration(
    const uint8_t bus_channel,
    uint32_t frame_id,
    std::string_view signal_name,
    std::string_view message) {

    throw std::invalid_argument(
        "Invalid CAN Signal configuration. Channel: "
        + std::to_string(bus_channel)
        + ", Frame ID: "
        + std::to_string(frame_id)
        + ", Signal: "
        + std::string(signal_name)
        + ". "
        + std::string(message));
}

void CanbusConfigurationValidator::Validate(const CanbusConfiguration& configuration, IFsService* sd_fs_service) {
    ValidateChannelType(configuration);
    ValidateIsExtendedId(configuration);
    ValidateBitrate(configuration);
    ValidateDataBitrate(configuration);

    ValidateMessages(configuration, sd_fs_service);
}

void CanbusConfigurationValidator::ValidateChannelType(const CanbusConfiguration& configuration) {
    for(const auto& [_, canbus_configuration] : configuration.channel_configurations) {
        if(canbus_configuration.type != CanbusType::CLASSICAL_CAN && canbus_configuration.type != CanbusType::CANFD)
            InvalidCanbusConfiguration(canbus_configuration.bus_channel, "Invalid CAN bus type");
    }
}

void CanbusConfigurationValidator::ValidateIsExtendedId(const CanbusConfiguration& configuration) {
    for(const auto& [_, channel_configuration] : configuration.channel_configurations) {
        for(const auto& message_configuration : channel_configuration.message_configurations) {
            const uint32_t id_mask = channel_configuration.is_extended_id ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK;

            if((message_configuration->frame_id & ~id_mask) != 0)
                InvalidCanbusConfiguration(channel_configuration.bus_channel, "CAN frame ID exceeds the configured identifier width");
        }
    }
}

void CanbusConfigurationValidator::ValidateBitrate(const CanbusConfiguration& configuration) {
    for(const auto& [_, canbus_configuration] : configuration.channel_configurations) {
        if(!Canbus::IsBitrateSupported(canbus_configuration.type, canbus_configuration.bitrate))
            InvalidCanbusConfiguration(canbus_configuration.bus_channel, "Invalid CAN bus bitrate");
    }
}

void CanbusConfigurationValidator::ValidateDataBitrate(const CanbusConfiguration& configuration) {
    for(const auto& [_, canbus_configuration] : configuration.channel_configurations) {
        if(canbus_configuration.data_bitrate > 0 && canbus_configuration.type == CanbusType::CLASSICAL_CAN)
            InvalidCanbusConfiguration(canbus_configuration.bus_channel, "Classical CAN does not support data bitrate");

        if(canbus_configuration.data_bitrate > 0 && canbus_configuration.data_bitrate < canbus_configuration.bitrate)
            InvalidCanbusConfiguration(canbus_configuration.bus_channel, "Invalid CAN bus data bitrate. Data bitrate must be greater than or equal to bitrate");

        if(canbus_configuration.data_bitrate > 0 && !Canbus::IsBitrateSupported(canbus_configuration.type, canbus_configuration.data_bitrate))
            InvalidCanbusConfiguration(canbus_configuration.bus_channel, "CAN bus data bitrate is not supported");
    }
}

void CanbusConfigurationValidator::ValidateMessages(const CanbusConfiguration& configuration, IFsService* sd_fs_service) {
    for(const auto& [_, canbus_configuration] : configuration.channel_configurations) {
        ValidateMessageFrameId(canbus_configuration);
        ValidateMessageSendIntervalMs(canbus_configuration);
        ValidateMessageScriptPath(canbus_configuration, sd_fs_service);
        ValidateMessageName(canbus_configuration);
        ValidateMessageSize(canbus_configuration, canbus_configuration.type);

        ValidateSignals(canbus_configuration);
    }
}

void CanbusConfigurationValidator::ValidateMessageFrameId(const CanChannelConfiguration& channel_configuration) {
    std::unordered_set<uint32_t> frame_ids;

    for(const auto& message_configuration : channel_configuration.message_configurations) {
        if(frame_ids.contains(message_configuration->frame_id))
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "CAN channel cannot contain duplicate Frame IDs."
            );

        frame_ids.insert(message_configuration->frame_id);
    }
}

void CanbusConfigurationValidator::ValidateMessageSendIntervalMs(const CanChannelConfiguration& channel_configuration) {
    for(const auto& message_configuration : channel_configuration.message_configurations) {
        if(message_configuration->send_interval_ms.has_value() && message_configuration->send_interval_ms.value() <= 0)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Invalid send interval."
            );
    }
}

void CanbusConfigurationValidator::ValidateMessageScriptPath(const CanChannelConfiguration& channel_configuration, IFsService* sd_fs_service) {
    if(sd_fs_service == nullptr)
        return;

    for(const auto& message_configuration : channel_configuration.message_configurations) {
        if(!message_configuration->script_path.empty()) {
            if(!sd_fs_service->Exists(message_configuration->script_path))
                InvalidCanMessageConfiguration(
                    channel_configuration.bus_channel,
                    message_configuration->frame_id,
                    "Invalid script path."
                );
        }
    }
}

void CanbusConfigurationValidator::ValidateMessageName(const CanChannelConfiguration& channel_configuration) {
    static constexpr std::string_view valid_symbols = "_";

    for(const auto& message_configuration : channel_configuration.message_configurations) {
        if(message_configuration->name.empty())
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Name cannot be empty."
            );

        if(message_configuration->name.size() > 32)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Name cannot be longer than 32 characters."
            );

        if(!std::isalpha(message_configuration->name[0]) && valid_symbols.find(message_configuration->name[0]) == std::string_view::npos)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Name must start with a letter or an underscore."
            );

        if(!std::ranges::all_of(message_configuration->name, [&valid_symbols](char c) {
            return std::isalnum(c) || valid_symbols.find(c) != std::string_view::npos;})) {

            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Name must contain only letters, digits, and underscores."
            );
        }
    }
}

void CanbusConfigurationValidator::ValidateMessageSize(const CanChannelConfiguration& channel_configuration, CanbusType type) {
    for(const auto& message_configuration : channel_configuration.message_configurations) {
        if(message_configuration->message_size < 1)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Message size cannot be less than 1 byte."
            );

        if(type == CanbusType::CLASSICAL_CAN && message_configuration->message_size > 8)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Message size cannot be greater than 8 bytes for Classical CAN."
            );

        if(type == CanbusType::CANFD && message_configuration->message_size > 64)
            InvalidCanMessageConfiguration(
                channel_configuration.bus_channel,
                message_configuration->frame_id,
                "Message size cannot be greater than 64 bytes for CAN FD."
            );
    }
}

void CanbusConfigurationValidator::ValidateSignals(const CanChannelConfiguration& channel_configuration) {
    for(const auto& message_configuration : channel_configuration.message_configurations) {
        ValidateSignalStartBit(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalSizeBits(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalLayout(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalFactor(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalOffset(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalName(*message_configuration, channel_configuration.bus_channel);
        ValidateSignalUnit(*message_configuration, channel_configuration.bus_channel);
    }
}

void CanbusConfigurationValidator::ValidateSignalStartBit(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    for(const auto& signal : message_configuration.signal_configurations) {
        if(signal.start_bit >= message_configuration.message_size * 8)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Start bit cannot be greater than (message size * 8)."
            );
    }
}

void CanbusConfigurationValidator::ValidateSignalSizeBits(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    for(const auto& signal : message_configuration.signal_configurations) {
        if(signal.size_bits < 1)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Size bits cannot be less than 1."
            );

        if(signal.size_bits > message_configuration.message_size * 8)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Size bits cannot be greater than (message size * 8)."
            );
    }
}

void CanbusConfigurationValidator::ValidateSignalLayout(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    for(const auto& signal : message_configuration.signal_configurations) {
        if(!IsCanSignalByteOrderValid(signal.byte_order))
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Invalid byte order."
            );

        if(!CanSignalCodec::IsLayoutValid(signal, message_configuration.message_size))
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Signal does not fit into the message."
            );
    }
}

void CanbusConfigurationValidator::ValidateSignalFactor(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    for(const auto& signal : message_configuration.signal_configurations) {
        if(signal.factor == 0.0F)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Factor cannot be zero."
            );
    }
}

void CanbusConfigurationValidator::ValidateSignalOffset(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    // Nothing to validate
}

void CanbusConfigurationValidator::ValidateSignalName(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    static constexpr std::string_view valid_symbols = "_";
    std::unordered_set<std::string_view> signal_names;

    for(const auto& signal : message_configuration.signal_configurations) {
        if(signal_names.contains(signal.name))
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Cannot contain duplicate signal names. " + signal.name
            );

        signal_names.insert(signal.name);

        if(signal.name.empty())
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Name cannot be empty."
            );

        if(signal.name.size() > 32)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Name cannot be longer than 32 characters."
            );

        if(!std::isalpha(signal.name[0]) && valid_symbols.find(signal.name[0]) == std::string_view::npos)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Name must start with a letter or an underscore."
            );

        if(!std::ranges::all_of(signal.name, [&valid_symbols](char c) {
            return std::isalnum(c) || valid_symbols.find(c) != std::string_view::npos;})) {

            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Name must contain only letters, digits, and underscores."
            );
        }
    }
}

void CanbusConfigurationValidator::ValidateSignalUnit(const CanMessageConfiguration& message_configuration, uint8_t bus_channel) {
    for(const auto& signal : message_configuration.signal_configurations) {
        if(signal.name.empty())
            continue;

        if(signal.name.size() > 32)
            InvalidCanSignalConfiguration(
                bus_channel,
                message_configuration.frame_id,
                signal.name,
                "Unit cannot be longer than 32 characters."
            );
    }
}

} // namespace eerie_leap::domain::canbus_domain::configuration::parsers
