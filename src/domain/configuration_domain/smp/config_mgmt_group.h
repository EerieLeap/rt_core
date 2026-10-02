#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/smp/smp_group.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/threading/work_queue_task.h"

#include "domain/configuration_domain/services/configuration_service.h"

namespace eerie_leap::domain::configuration_domain::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;

/**
 * @brief SMP `config` group: lists the configuration types and reads or writes their CBOR in chunks.
 *
 * Storage reads, transfer buffer allocation and ConfigurationService::ApplyCborConfiguration() run
 * on the configuration work queue while the MCUmgr handler waits, so the MCUmgr stack stays small
 * and never unwinds an exception. One write session and one read snapshot exist at a time; both
 * are freed on completion, on error, or after CONFIG_EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS of
 * inactivity. A client whose answer to the first write got lost may send that request again: an
 * exact repeat gets the session's token instead of BUSY. A written configuration is stored as
 * received, so `crc` reports the writer's CRC until the configuration changes locally.
 */
class ConfigMgmtGroup : public SmpGroup<ConfigMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        LIST = 0,  ///< Read: `{types: [{type, max}]}`.
        CRC = 1,   ///< Read `{type}`: `{type, len, crc}` of the stored configuration.
        READ = 2,  ///< Read `{type, off}`: `{off, data}`, plus `len` and `crc` at `off = 0`.
        WRITE = 3, ///< Write `{type, off, data}`, plus `len` and `crc` at `off = 0`, `tok` after: `{off, tok}`.
    };

    /** @brief Group error codes, returned in the SMP v2 `err` map. */
    enum class Error : uint16_t {
        OK = 0,
        UNKNOWN = 1,
        UNKNOWN_TYPE = 2, ///< No configuration of that type is registered.
        TOO_LARGE = 3,    ///< The length exceeds the type's cap, or the data exceeds the length.
        BAD_OFFSET = 4,   ///< Not the next offset (the response carries it), or no read snapshot.
        BAD_TOKEN = 5,    ///< The token or type does not match the open write session.
        BUSY = 6,         ///< Another client has a write session open.
        CRC_MISMATCH = 7,
        APPLY_FAILED = 8, ///< The configuration was rejected, or could not be stored; `msg` gives the reason, if known.
        NO_MEMORY = 9,    ///< The transfer buffer could not be allocated.
    };

    /// Longest `msg` of an APPLY_FAILED; a longer reason is truncated.
    static constexpr size_t MAX_REASON_LENGTH = 95;

private:
    enum class JobKind : uint8_t {
        STAGE,
        APPLY,
        SNAPSHOT,
    };

    struct Job {
        JobKind kind;
        ConfigurationService::Type type;
        uint32_t size = 0;
        uint32_t crc = 0;
        Error result = Error::UNKNOWN;
    };

    struct WriteSession {
        ConfigurationService::Type type{};
        uint32_t token = 0; // 0 while no session is open
        uint32_t crc = 0;
        size_t received = 0;
        std::pmr::vector<uint8_t> data;
        int64_t last_activity_ms = 0;
    };

    struct ReadSnapshot {
        ConfigurationService::Type type{};
        bool is_valid = false;
        uint32_t crc = 0;
        std::pmr::vector<uint8_t> data;
        int64_t last_activity_ms = 0;
    };

    static const mgmt_handler HANDLERS[];

    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    // Guards session_ and snapshot_ between the MCUmgr handlers and the idle task.
    k_mutex lock_{};
    WriteSession session_;
    ReadSnapshot snapshot_;

    std::optional<WorkQueueTask<ConfigMgmtGroup>> job_task_;
    std::optional<WorkQueueTask<ConfigMgmtGroup>> idle_task_;
    Job* job_ = nullptr;
    k_sem job_done_{};
    // Written by an APPLY job while HandleWrite() waits for it; kept off the small MCUmgr stack.
    std::array<char, MAX_REASON_LENGTH + 1> apply_reason_{};

    int HandleList(smp_streamer* ctxt);
    int HandleCrc(smp_streamer* ctxt);
    int HandleRead(smp_streamer* ctxt);
    int HandleWrite(smp_streamer* ctxt);

    /** @param message Sent as `msg`, unless empty. */
    int Fail(smp_streamer* ctxt, Error error, std::string_view message = {});
    /** @param token The session's token, which answers its first chunk; 0 leaves it out. */
    static int RespondToWrite(smp_streamer* ctxt, size_t received, uint32_t token);
    bool IsKnownType(uint32_t type) const;
    void OnActivity();

    Error RunJob(Job& job);
    WorkQueueTaskResult ProcessJob();
    Error ExecuteJob(Job& job);
    WorkQueueTaskResult ProcessIdle();

    void CloseSessionLocked();
    void ReleaseSnapshotLocked();

    static int TranslateError(uint16_t error);

public:
    /**
     * @param config_work_queue_thread Runs the heavy parts of the commands, as for any other
     *        configuration change.
     */
    ConfigMgmtGroup(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread);
    ~ConfigMgmtGroup();

    /** @brief Creates the work queue tasks; needs a running work queue. Call before Register(). */
    bool Initialize();
};

} // namespace eerie_leap::domain::configuration_domain::smp
