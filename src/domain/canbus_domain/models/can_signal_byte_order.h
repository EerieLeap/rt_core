#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace eerie_leap::domain::canbus_domain::models {

using namespace std::string_view_literals;

// Bit numbering of a CAN signal inside a frame, as defined by DBC.
enum class CanSignalByteOrder : uint8_t {
    LITTLE_ENDIAN_INTEL,
    BIG_ENDIAN_MOTOROLA
};

constexpr const std::array CAN_SIGNAL_BYTE_ORDER_NAMES = { "LITTLE_ENDIAN_INTEL"sv, "BIG_ENDIAN_MOTOROLA"sv };

constexpr bool IsCanSignalByteOrderValid(CanSignalByteOrder byte_order) {
    return std::to_underlying(byte_order) < CAN_SIGNAL_BYTE_ORDER_NAMES.size();
}

inline const char* GetCanSignalByteOrderName(CanSignalByteOrder byte_order) {
    if(!IsCanSignalByteOrderValid(byte_order))
        return "UNKNOWN";

    return CAN_SIGNAL_BYTE_ORDER_NAMES[std::to_underlying(byte_order)].data();
}

inline CanSignalByteOrder GetCanSignalByteOrder(std::string_view name) {
    for(size_t i = 0; i < size(CAN_SIGNAL_BYTE_ORDER_NAMES); ++i) {
        if(CAN_SIGNAL_BYTE_ORDER_NAMES[i] == name)
            return static_cast<CanSignalByteOrder>(i);
    }

    throw std::runtime_error("Invalid CAN signal byte order.");
}

} // namespace eerie_leap::domain::canbus_domain::models
