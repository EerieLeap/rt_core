#pragma once

#include <memory_resource>
#include <cstdint>
#include <string>

#include "utilities/string/string_helpers.h"

#include "can_signal_byte_order.h"

namespace eerie_leap::domain::canbus_domain::models {

using eerie_leap::utilities::string::StringHelpers;

struct CanSignalConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    uint32_t start_bit = 0;
    uint32_t size_bits = 0;
    float factor = 1.0F;
    float offset = 0.0F;
    CanSignalByteOrder byte_order = CanSignalByteOrder::LITTLE_ENDIAN_INTEL;
    bool is_signed = true;

    std::pmr::string name;
    std::pmr::string unit;

    size_t name_hash = 0;

    CanSignalConfiguration(std::allocator_arg_t, allocator_type alloc)
        : name(alloc), unit(alloc) {}

    CanSignalConfiguration(const CanSignalConfiguration&) = delete;
	CanSignalConfiguration& operator=(const CanSignalConfiguration&) noexcept = default;
	CanSignalConfiguration& operator=(CanSignalConfiguration&&) noexcept = default;
	CanSignalConfiguration(CanSignalConfiguration&&) noexcept = default;
	~CanSignalConfiguration() = default;

    CanSignalConfiguration(CanSignalConfiguration&& other, allocator_type alloc)
        : start_bit(other.start_bit),
        size_bits(other.size_bits),
        factor(other.factor),
        offset(other.offset),
        byte_order(other.byte_order),
        is_signed(other.is_signed),
        name(std::move(other.name), alloc),
        unit(std::move(other.unit), alloc),
        name_hash(other.name_hash) {}

    void SetName(std::string_view value) {
        name.assign(value);
        name_hash = value.empty() ? 0 : StringHelpers::GetHash(value);
    }
};

} // namespace eerie_leap::domain::canbus_domain::models
