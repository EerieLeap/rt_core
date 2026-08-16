#pragma once

#include <cstdint>
#include <array>
#include <stdexcept>
#include <utility>
#include <string_view>

namespace eerie_leap::domain::canbus_domain::models {

using namespace std::string_view_literals;

// Bit numbering of a CAN signal inside a frame, as defined by DBC.
enum class CanSignalByteOrder : uint8_t {
    LITTLE_ENDIAN_INTEL,
    BIG_ENDIAN_MOTOROLA
};

constexpr const std::array CanSignalByteOrderNames = {
    "LITTLE_ENDIAN_INTEL"sv,
    "BIG_ENDIAN_MOTOROLA"sv
};

constexpr bool IsCanSignalByteOrderValid(CanSignalByteOrder byte_order) {
    return std::to_underlying(byte_order) < CanSignalByteOrderNames.size();
}

inline const char* GetCanSignalByteOrderName(CanSignalByteOrder byte_order) {
    if(!IsCanSignalByteOrderValid(byte_order))
        return "UNKNOWN";

    return CanSignalByteOrderNames[std::to_underlying(byte_order)].data();
}

inline CanSignalByteOrder GetCanSignalByteOrder(std::string_view name) {
    for(size_t i = 0; i < size(CanSignalByteOrderNames); ++i) {
        if(CanSignalByteOrderNames[i] == name)
            return static_cast<CanSignalByteOrder>(i);
    }

    throw std::runtime_error("Invalid CAN signal byte order.");
}

} // namespace eerie_leap::domain::canbus_domain::models
