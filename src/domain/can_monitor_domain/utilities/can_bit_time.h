#pragma once

#include <cstddef>
#include <cstdint>

#include "domain/can_monitor_domain/utilities/can_record.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

/** @brief The bit times of one frame, split by the bitrate they run at. */
struct CanBitTimeResult {
    /** @brief Bits at the nominal bitrate. */
    uint32_t nominal_bits = 0;
    /** @brief Bits at the data bitrate (0 when the frame has no faster data phase). */
    uint32_t data_bits = 0;
    /** @brief The frame's time in ns, each part rounded to the nearest ns. */
    uint64_t time_ns = 0;
};

/**
 * @brief The time a frame takes on the bus without stuffing, which is what a busy-time statistic sums.
 *
 * ns, so that a caller adding frame after frame keeps the error to a ns a frame instead of a
 * truncated µs.
 *
 * Derivation (ISO 11898-1), in bit times:
 *
 * - Classical, 11-bit ID: SOF 1, ID 11, RTR 1, IDE 1, r0 1, DLC 4, data 8n, CRC 15, CRC delimiter 1,
 *   ACK 1, ACK delimiter 1, EOF 7, interframe space 3 = 47 + 8n.
 * - Classical, 29-bit ID: SOF 1, base ID 11, SRR 1, IDE 1, extension 18, RTR 1, r1 1, r0 1, DLC 4,
 *   then as above = 67 + 8n.
 * - A remote frame has no data field: n = 0.
 * - FD arbitration phase, SOF to BRS, at the nominal bitrate: SOF 1, ID 11, RRS 1, IDE 1, FDF 1,
 *   res 1, BRS 1 = 17 with an 11-bit ID; SOF 1, base ID 11, SRR 1, IDE 1, extension 18, r1 1, FDF 1,
 *   res 1, BRS 1 = 36 with a 29-bit one.
 * - FD data phase (at the data bitrate when BRS is set): ESI 1, DLC 4, data 8n, then the CRC field:
 *   stuff count 4, CRC 17 bits up to 16 data bytes and 21 above, and the fixed stuff bits (one at
 *   the field's start and one after every 4 bits of it: 6 for 4 + 17 bits, 7 for 4 + 21).
 *   That is 1 + 4 + 8n + 4 + 17 + 6 = 32 + 8n for n <= 16, and 1 + 4 + 8n + 4 + 21 + 7 = 37 + 8n above.
 * - FD end, back at the nominal bitrate: CRC delimiter 1, ACK 1, ACK delimiter 1, EOF 7,
 *   interframe space 3 = 13.
 *
 * Without BRS an FD frame runs at the nominal bitrate throughout.
 */
class CanBitTime {
public:
    /**
     * @param payload_bytes The data field's bytes; ignored (0) for a remote frame.
     * @param nominal_bitrate bit/s; 0 gives a time of 0.
     * @param data_bitrate bit/s of the data phase; 0 means the nominal one.
     */
    static CanBitTimeResult Compute(
        bool ide,
        bool fdf,
        bool brs,
        bool rtr,
        size_t payload_bytes,
        uint32_t nominal_bitrate,
        uint32_t data_bitrate
    );

    /** @brief The same for a frame as the records see it: the data field's length comes from the DLC. */
    static CanBitTimeResult Compute(const CanRecordFrame& frame, uint32_t nominal_bitrate, uint32_t data_bitrate);
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities
