#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::services {

// Counters of the log writer since Start(), across rotations. Plain integers written by the writer
// thread and read for display; a torn read costs nothing more than a stale number.
struct LogWriterStats {
    uint32_t ticks = 0;
    uint32_t readings = 0;       // value records written
    uint32_t frames = 0;         // raw CAN frames written
    uint32_t frames_dropped = 0; // raw CAN frames the frame queue dropped since Start()
    uint32_t syncs = 0;
    uint32_t rotations = 0; // files finalized and replaced because they reached max_log_size_mb
    uint32_t overruns = 0;  // deadlines skipped because a tick ran past them
    uint32_t write_errors = 0;
    uint32_t tick_last_us = 0;
    uint32_t tick_max_us = 0;
    uint32_t stream_writes = 0; // fs_write() calls made by the stream buffers
    uint64_t bytes = 0;         // bytes the logger wrote, headers included
};

} // namespace eerie_leap::domain::logging_domain::services
