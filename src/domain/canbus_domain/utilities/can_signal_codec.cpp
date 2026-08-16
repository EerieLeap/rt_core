#include <algorithm>
#include <cmath>

#include "can_signal_codec.h"

namespace eerie_leap::domain::canbus_domain::utilities {

using eerie_leap::domain::canbus_domain::models::CanSignalByteOrder;

// Position of the byte holding the last bit the signal occupies.
size_t CanSignalCodec::GetLastByteIndex(const CanSignalConfiguration& signal) {
    if(signal.byte_order == CanSignalByteOrder::LITTLE_ENDIAN_INTEL)
        return (signal.start_bit + signal.size_bits - 1) / 8;

    const uint32_t bits_in_start_byte = (signal.start_bit % 8) + 1;
    if(signal.size_bits <= bits_in_start_byte)
        return signal.start_bit / 8;

    return (signal.start_bit / 8) + ((signal.size_bits - bits_in_start_byte + 7) / 8);
}

uint64_t CanSignalCodec::SignalMask(uint32_t size_bits) {
    return size_bits >= MAX_SIGNAL_SIZE_BITS ? UINT64_MAX : (1ULL << size_bits) - 1ULL;
}

template <typename TBitHandler>
void CanSignalCodec::ForEachSignalBit(const CanSignalConfiguration& signal, TBitHandler&& handler) {
    if(signal.byte_order == CanSignalByteOrder::LITTLE_ENDIAN_INTEL) {
        for(uint32_t i = 0; i < signal.size_bits; ++i)
            handler(signal.start_bit + i, i);

        return;
    }

    uint32_t bit_position = signal.start_bit;
    for(uint32_t i = 0; i < signal.size_bits; ++i) {
        handler(bit_position, signal.size_bits - 1 - i);

        if(bit_position % 8 == 0)
            bit_position += 15;
        else
            bit_position--;
    }
}

bool CanSignalCodec::IsLayoutValid(const CanSignalConfiguration& signal, size_t data_size) {
    if(signal.size_bits == 0 || signal.size_bits > MAX_SIGNAL_SIZE_BITS)
        return false;

    if(signal.start_bit >= data_size * 8)
        return false;

    return GetLastByteIndex(signal) < data_size;
}

std::optional<float> CanSignalCodec::Decode(const CanSignalConfiguration& signal, std::span<const uint8_t> data) {
    if(!IsLayoutValid(signal, data.size()))
        return std::nullopt;

    uint64_t raw = 0;
    ForEachSignalBit(signal, [&raw, data](uint32_t frame_bit, uint32_t signal_bit) {
        if((data[frame_bit / 8] >> (frame_bit % 8)) & 0x01U)
            raw |= 1ULL << signal_bit;
    });

    double physical = 0.0;
    if(signal.is_signed) {
        const uint64_t sign_mask = 1ULL << (signal.size_bits - 1);
        if(raw & sign_mask)
            raw |= ~SignalMask(signal.size_bits);

        physical = static_cast<double>(static_cast<int64_t>(raw));
    } else {
        physical = static_cast<double>(raw);
    }

    return static_cast<float>(physical * signal.factor + signal.offset);
}

bool CanSignalCodec::Encode(const CanSignalConfiguration& signal, float value, std::span<uint8_t> data) {
    if(!IsLayoutValid(signal, data.size()))
        return false;

    if(signal.factor == 0.0F || !std::isfinite(value))
        return false;

    const double scaled = (static_cast<double>(value) - signal.offset) / signal.factor;
    const uint64_t mask = SignalMask(signal.size_bits);

    // Saturate in double space, the limits are chosen to stay castable to the target integer type.
    uint64_t raw = 0;
    if(signal.is_signed) {
        const double limit = std::ldexp(1.0, static_cast<int>(signal.size_bits) - 1);
        const double clamped = std::clamp(scaled, -limit, std::nextafter(limit, 0.0));

        raw = static_cast<uint64_t>(static_cast<int64_t>(clamped)) & mask;
    } else {
        const double limit = std::ldexp(1.0, static_cast<int>(signal.size_bits));
        const double clamped = std::clamp(scaled, 0.0, std::nextafter(limit, 0.0));

        raw = static_cast<uint64_t>(clamped) & mask;
    }

    ForEachSignalBit(signal, [raw, data](uint32_t frame_bit, uint32_t signal_bit) {
        const uint8_t frame_bit_mask = static_cast<uint8_t>(1U << (frame_bit % 8));

        if((raw >> signal_bit) & 0x01ULL)
            data[frame_bit / 8] |= frame_bit_mask;
        else
            data[frame_bit / 8] &= static_cast<uint8_t>(~frame_bit_mask);
    });

    return true;
}

} // namespace eerie_leap::domain::canbus_domain::utilities
