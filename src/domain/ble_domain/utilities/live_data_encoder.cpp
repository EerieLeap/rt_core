#include <bit>

#include <zephyr/sys/byteorder.h>

#include "live_data_encoder.h"

namespace eerie_leap::domain::ble_domain::utilities {

size_t LiveDataEncoder::Encode(
    uint8_t sequence, uint32_t time_ms, std::span<const std::optional<float>> values, std::span<uint8_t> out) {

    if(values.size() > MAX_VALUES || out.size() < GetMaxSize(values.size()))
        return 0;

    out[0] = VERSION;
    out[1] = sequence;
    sys_put_le32(time_ms, &out[2]);

    size_t size = HEADER_SIZE;
    uint8_t count = 0;

    for(size_t index = 0; index < values.size(); index++) {
        if(!values[index].has_value())
            continue;

        out[size] = static_cast<uint8_t>(index);
        sys_put_le32(std::bit_cast<uint32_t>(*values[index]), &out[size + 1]);
        size += ENTRY_SIZE;
        count++;
    }

    out[HEADER_SIZE - 1] = count;

    return size;
}

} // namespace eerie_leap::domain::ble_domain::utilities
