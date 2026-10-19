#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace eerie_leap::domain::logging_domain::models {

// What the log writer can tell about the session it runs, for the SMP logging group's STATUS. Header
// only: the group is built in both roles, and only the writer role has a writer to ask.
struct LogSessionInfo {
    // Longer than any name LogFileNaming builds (the prefix plus 26 characters).
    static constexpr size_t MAX_FILE_NAME_LENGTH = 63;

    // The session's start (Start()), in milliseconds since the Unix epoch, UTC.
    int64_t since_ms = 0;

    // The open file's asset name in the `logs` kind (no directory) and its bytes so far, headers
    // included. The name is empty while no file is open: between the two files of a rotation, or after
    // a failure closed it.
    std::array<char, MAX_FILE_NAME_LENGTH + 1> file_name{};
    uint64_t file_bytes = 0;
};

// The running session, or null when none runs. Callable from any thread, including the MCUmgr work
// queue: no allocation, no file I/O, no lock held across either.
using LogSessionProvider = std::function<std::optional<LogSessionInfo>()>;

} // namespace eerie_leap::domain::logging_domain::models
