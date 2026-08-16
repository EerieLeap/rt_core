#pragma once

#include <memory_resource>
#include <cstdint>
#include <string>
#include <vector>

#include "subsys/canbus/canbus_type.h"

#include "can_message_configuration.h"

namespace eerie_leap::domain::canbus_domain::models {

using eerie_leap::subsys::canbus::CanbusType;

struct CanChannelConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    CanbusType type = CanbusType::NONE;
    bool is_extended_id = false;
    uint8_t bus_channel = 0;
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    std::pmr::vector<std::shared_ptr<CanMessageConfiguration>> message_configurations;

    CanChannelConfiguration(std::allocator_arg_t, allocator_type alloc)
        : message_configurations(alloc) {}

    CanChannelConfiguration(const CanChannelConfiguration&) = delete;
	CanChannelConfiguration& operator=(const CanChannelConfiguration&) noexcept = default;
	CanChannelConfiguration& operator=(CanChannelConfiguration&&) noexcept = default;
	CanChannelConfiguration(CanChannelConfiguration&&) noexcept = default;
	~CanChannelConfiguration() = default;

    CanChannelConfiguration(CanChannelConfiguration&& other, allocator_type alloc)
        : type(other.type),
        is_extended_id(other.is_extended_id),
        bus_channel(other.bus_channel),
        bitrate(other.bitrate),
        data_bitrate(other.data_bitrate),
        message_configurations(std::move(other.message_configurations), alloc) {}
};

} // namespace eerie_leap::domain::canbus_domain::models
