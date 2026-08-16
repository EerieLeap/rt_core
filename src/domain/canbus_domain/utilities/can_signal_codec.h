#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "domain/canbus_domain/models/can_signal_configuration.h"

namespace eerie_leap::domain::canbus_domain::utilities {

using eerie_leap::domain::canbus_domain::models::CanSignalConfiguration;

// Bit level packing/unpacking of a single CAN signal, following DBC bit numbering semantics.
class CanSignalCodec {
private:
    static constexpr uint32_t MAX_SIGNAL_SIZE_BITS = 64;

    static size_t GetLastByteIndex(const CanSignalConfiguration& signal);
    static uint64_t SignalMask(uint32_t size_bits);

    // Walks the frame bit positions the signal occupies, from its least to most significant bit.
    template <typename TBitHandler>
    static void ForEachSignalBit(const CanSignalConfiguration& signal, TBitHandler&& handler);

public:
    static bool IsLayoutValid(const CanSignalConfiguration& signal, size_t data_size);

    static std::optional<float> Decode(const CanSignalConfiguration& signal, std::span<const uint8_t> data);
    static bool Encode(const CanSignalConfiguration& signal, float value, std::span<uint8_t> data);
};

} // namespace eerie_leap::domain::canbus_domain::utilities
