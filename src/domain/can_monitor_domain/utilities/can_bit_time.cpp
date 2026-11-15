#include "domain/can_monitor_domain/utilities/can_bit_time.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

namespace {

constexpr uint64_t NS_PER_SECOND = 1000000000;

/** @brief bits at bitrate, in ns rounded to the nearest. A frame has under 1,000 bits, so this can't overflow. */
uint64_t BitsToNs(uint32_t bits, uint32_t bitrate) {
    if(bits == 0 || bitrate == 0)
        return 0;

    return (static_cast<uint64_t>(bits) * NS_PER_SECOND + bitrate / 2) / bitrate;
}

} // namespace

CanBitTimeResult CanBitTime::Compute(
    bool ide,
    bool fdf,
    bool brs,
    bool rtr,
    size_t payload_bytes,
    uint32_t nominal_bitrate,
    uint32_t data_bitrate
) {
    const uint32_t n = rtr ? 0 : static_cast<uint32_t>(payload_bytes);

    CanBitTimeResult result;

    if(!fdf) {
        result.nominal_bits = (ide ? 67 : 47) + 8 * n;
    } else {
        const uint32_t arbitration = ide ? 36 : 17;
        const uint32_t data = (n <= 16 ? 32 : 37) + 8 * n;
        constexpr uint32_t END = 13;

        if(brs) {
            result.nominal_bits = arbitration + END;
            result.data_bits = data;
        } else {
            result.nominal_bits = arbitration + data + END;
        }
    }

    const uint32_t data_rate = data_bitrate == 0 ? nominal_bitrate : data_bitrate;
    result.time_ns = BitsToNs(result.nominal_bits, nominal_bitrate) + BitsToNs(result.data_bits, data_rate);

    return result;
}

CanBitTimeResult CanBitTime::Compute(const CanRecordFrame& frame, uint32_t nominal_bitrate, uint32_t data_bitrate) {
    return Compute(
        frame.ide, frame.fdf, frame.brs, frame.rtr, PayloadLength(frame.dlc, frame.fdf, frame.rtr), nominal_bitrate,
        data_bitrate
    );
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
