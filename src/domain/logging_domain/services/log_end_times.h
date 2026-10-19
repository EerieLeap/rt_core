#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/fs/services/i_fs_service.h"
#include "utilities/string/static_string.hpp"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::utilities::string::StaticString;

/**
 * Where each finished log file ended, so a client can tell its duration without reading the file: a
 * finalized MDF4 header holds no end time. The writer records a file's end when it closes the file
 * with a successful finalize.
 *
 * The index lives in memory and in one text file, `logs/end_times.txt` (LogFileNaming::END_TIMES_FILE_NAME)
 * on the log volume, a line per file: `<end_ms> <file name>\n`, end_ms in milliseconds since the Unix
 * epoch, UTC, the file name without its directory. Record() appends a line. The file is rewritten, in
 * the order of the entries, by Load() when it dropped anything, and by Record() once the lines
 * outnumber the cap twice, so it stays a few tens of KiB.
 *
 * Logs are never renamed, so an entry stays right for its name. The entry of a file that is gone is
 * ignored by Find() as nothing asks for it, and dropped when the index is loaded next. At most
 * MAX_ENTRIES entries are kept; the oldest recorded leave first.
 *
 * Load() and Record() are for one thread at a time (the writer's, once it runs) and do file I/O;
 * Find() is for any thread, takes a lock that is never held across I/O and allocates nothing.
 * No failure of the index is thrown: it is logged, and the index goes on in memory.
 */
class LogEndTimes {
public:
    static constexpr size_t MAX_ENTRIES = 256;
    static constexpr size_t MAX_NAME_LENGTH = 63;
    /// A file larger than this is taken as corrupt.
    static constexpr size_t MAX_FILE_SIZE = 64 * 1024;

private:
    struct Entry {
        StaticString<MAX_NAME_LENGTH> name;
        int64_t end_ms = 0;
    };

    std::shared_ptr<IFsService> fs_service_;
    mutable k_mutex mutex_{};
    std::pmr::vector<Entry> entries_;
    // Lines in the file; only Load() and Record() touch it.
    size_t file_lines_ = 0;
    // The mount the entries were read from; null before the first Load() that found a volume.
    std::optional<uint32_t> loaded_generation_;

    [[nodiscard]] static std::string FilePath();
    // Under the lock: puts or replaces an entry, dropping the oldest at the cap.
    void Put(std::string_view name, int64_t end_ms);
    bool Rewrite();

public:
    explicit LogEndTimes(std::shared_ptr<IFsService> fs_service);

    /**
     * Reads the file once per mount of the volume (a call for a mount already read does nothing; a
     * volume that went away and came back may have changed, so it is read again), ignoring entries whose log is not on the volume, and rewrites it when that
     * dropped anything or a line was unreadable. A missing file is an empty index; a corrupt one
     * starts what it can read and is rewritten.
     */
    void Load();

    /** Remembers that @p name ended at @p end_ms, in memory and on the volume. */
    void Record(std::string_view name, int64_t end_ms);

    /** The end of the file @p name, or null when not recorded. */
    [[nodiscard]] std::optional<int64_t> Find(std::string_view name) const;

    [[nodiscard]] size_t Size() const;
};

} // namespace eerie_leap::domain::logging_domain::services
