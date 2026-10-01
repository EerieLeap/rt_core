#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace eerie_leap::domain::ble_domain::utilities {

/**
 * @brief Encodes a `live` notification: `[version][seq][t_ms: u32 LE][n]`, then n × `[index][value: f32 LE]`.
 *
 * `index` is the sensor's position in the subscription; sensors without a value are left out.
 */
class LiveDataEncoder {
public:
    static constexpr uint8_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = 7;
    static constexpr size_t ENTRY_SIZE = 5;
    static constexpr size_t MAX_VALUES = UINT8_MAX;

    static constexpr size_t GetMaxSize(size_t value_count) { return HEADER_SIZE + value_count * ENTRY_SIZE; }

    /**
     * @param out At least GetMaxSize(values.size()) bytes.
     * @return The encoded size; 0 if @p out is too small or there are more than MAX_VALUES values.
     */
    static size_t Encode(
        uint8_t sequence, uint32_t time_ms, std::span<const std::optional<float>> values, std::span<uint8_t> out);
};

} // namespace eerie_leap::domain::ble_domain::utilities
