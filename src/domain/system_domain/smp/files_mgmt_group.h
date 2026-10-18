#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/smp/smp_group.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

/**
 * @brief SMP `files` group: lists a directory of the SD card, so a client can pick a script.
 *
 * The directory is read on the configuration work queue while the MCUmgr handler waits, so the
 * MCUmgr stack stays small and never unwinds an exception. Each request reads the directory again
 * from its start: paging keeps no state on the unit and holds together while the directory does
 * not change. A unit without an SD service, or without a card mounted, answers NO_CARD.
 */
class FilesMgmtGroup : public SmpGroup<FilesMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read `{dir, off}` (both optional): `{total, off, entries: [{name, size, type}]}`, the
        /// entries of `dir` from `off`, in the file system's order, as many as fit into one
        /// response. `dir` is relative to the card's mount point, as a `script_path` is; the card's
        /// root when absent. `type` is 0 for a file and 1 for a directory, whose `size` is 0.
        /// Request the rest from `off` plus the entries listed; an empty page or `total` ends it.
        LIST = 0,
    };

    /** @brief Group error codes, returned in the SMP v2 `err` map. */
    enum class Error : uint16_t {
        OK = 0,
        UNKNOWN = 1,
        NO_CARD = 2,   ///< No SD service, or no card mounted.
        NOT_FOUND = 3, ///< No such directory.
        BAD_PATH = 4,  ///< Absolute, a `..` segment, a backslash or NUL, longer than MAX_DIR_LENGTH, or a file.
        IO_ERROR = 5,  ///< The card could not be read.
        NO_MEMORY = 6, ///< The entries could not be stored.
    };

    /// Entries read for one request: more than one response holds.
    static constexpr size_t MAX_PAGE_ENTRIES = 48;
    /// Longest `dir`, in bytes.
    static constexpr size_t MAX_DIR_LENGTH = 128;

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<const IFsService> sd_fs_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::optional<WorkQueueTask<FilesMgmtGroup>> job_task_;
    k_sem job_done_{};

    // The job's input and output, touched only by HandleList() and the job it waits for. The
    // directory points into the request, which stays alive while the handler waits.
    std::string_view job_directory_;
    uint32_t job_offset_ = 0;
    std::vector<DirectoryEntry> page_;
    size_t count_ = 0;
    Error job_result_ = Error::OK;

    int HandleList(smp_streamer* ctxt);

    static int Fail(smp_streamer* ctxt, Error error);
    static bool IsBadPath(std::string_view directory);
    void ReleasePage();

    WorkQueueTaskResult ProcessJob();

    /** @brief Maps the result of IFsService::ListDirectory() to a group error. */
    static Error ToError(int rc);
    static int TranslateError(uint16_t error);

public:
    /**
     * @param sd_fs_service The SD card; nullptr on a unit without one, which answers NO_CARD.
     * @param config_work_queue_thread Reads the directory, as it runs the other configuration work.
     */
    FilesMgmtGroup(
        std::shared_ptr<const IFsService> sd_fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread
    );
    ~FilesMgmtGroup();

    /** @brief Creates the work queue task; needs a running work queue. Call before Register(). */
    bool Initialize();
};

} // namespace eerie_leap::domain::system_domain::smp
