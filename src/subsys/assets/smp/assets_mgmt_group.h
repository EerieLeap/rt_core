#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"
#include "subsys/smp/smp_group.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"

namespace eerie_leap::subsys::assets::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

/**
 * @brief SMP `assets` group: lists the asset kinds a unit serves, and lists, downloads, uploads and
 *        deletes their files.
 *
 * Every handler only builds a small job and waits for it. The job decodes the request, checks it,
 * does the file I/O and encodes the response on the configuration work queue, so the MCUmgr stack
 * stays small and never unwinds an exception, and the group's state needs no lock: it is touched
 * only by jobs and the idle task, which run on that queue.
 *
 * The kinds come from the AssetsManager registry, each with its own store, extensions, access
 * mask, size cap and in-use check; a kind that is not registered is UNKNOWN_KIND. A name must pass
 * AssetStore::IsValidName() for the kind. File I/O fails with UNAVAILABLE while the kind's volume is
 * not mounted.
 *
 * One download is open at a time (the read session). It is closed on completion, on an error that
 * invalidates it, when another file is asked for, and after
 * CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS of inactivity. A running CRC32 covers the file from
 * its start up to the farthest byte read. Closing a session keeps that CRC in a checkpoint, so that
 * a client which comes back to the file at an offset past its start continues without the unit
 * reading it again. A download from the start never uses a checkpoint: the file may have changed.
 *
 * One upload is open at a time (the upload session), with a token, as in the `config` group. Its
 * data goes to a staged file on the kind's volume (AssetStore::BeginWrite()), which no listing
 * shows; the last chunk checks the CRC32 and renames it onto the asset. A failure of the file
 * system, a CRC mismatch, data beyond `len`, a file that went into use, a kind or volume that went
 * away and CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS of inactivity end the upload: the staged
 * file is deleted and the asset stays as it was. BAD_OFFSET, BAD_TOKEN and BUSY answer a request
 * and leave the session as it was. A client whose answer got lost may send a chunk again: the first
 * chunk, while nothing came after it, gets the session's token instead of BUSY; the last one, after
 * the commit, gets the same answer again; and a file sent in one chunk, which the unit stored, gets
 * its token again for the same request without one.
 */
class AssetsMgmtGroup : public SmpGroup<AssetsMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read, empty body: `{kinds: [{kind, name, dir, access, max, ext: [tstr...], avail}]}`,
        /// ordered by `kind`. `name` is the kind's name, `dir` its directory on the volume, `access`
        /// the AssetAccess bits, `max` the largest upload (0 for none), `ext` the extensions with their
        /// dot and `avail` whether the volume is mounted now.
        KINDS = 0,
        /// Read `{kind, off?}`: `{entries: [{name, size}], next?}`. The files of the kind with a valid
        /// name from directory position `off` (0 when absent), as many as fit into the response.
        /// Request the rest from `next` until it is absent: a page can be empty, and `next` present,
        /// when the entries around it are no assets. Needs no access bit.
        LIST = 1,
        /// Read `{kind, name, off}`: `{off, data}`, plus `len` (the size of the file) at `off = 0`,
        /// and `crc` (CRC32/IEEE of the whole file) in the chunk that ends at `len`. A request at
        /// `off = len` answers empty data and `crc`. Every chunk but the last has the same length,
        /// whatever its offset, so a client can request several at multiples of the first one's.
        READ = 2,
        /// Write `{kind, name, off, data}`, plus `len` (the size of the file) and `crc` (CRC32/IEEE of
        /// the whole file) at `off = 0`, `tok` after: `{off, tok}`, the offset to send next (`len` once
        /// the file is stored), and the session's token in the answer to `off = 0` only. A chunk must
        /// start where the last one ended: otherwise the error carries `off`, where to go on.
        ///
        /// The first chunk is checked in this order: the kind, its WRITE bit, the name, the volume,
        /// the size (TOO_LARGE), whether the file is in use, whether an upload is open (BUSY, unless it
        /// is this chunk sent again, or the owner's token starts it over) and the room on the volume
        /// (NO_SPACE). The owner's restart aborts its old upload before the room is looked at, so the
        /// room its staged file held counts. A staged file left on the volume with no upload open is
        /// stale and deleted before the room is looked at.
        ///
        /// The last chunk can answer IN_USE as well: the file is asked again whether it is in use just
        /// before it is replaced, and the upload ends if it is.
        WRITE = 3,
        /// Write `{kind, name}`: `{}`.
        DELETE = 4,
    };

    /** @brief Group error codes, returned in the SMP v2 `err` map. */
    enum class Error : uint16_t {
        OK = 0,
        UNKNOWN = 1,
        UNKNOWN_KIND = 2, ///< No provider registered for the kind.
        UNAVAILABLE = 3,  ///< The kind's volume is not mounted (or the unit has none).
        NOT_FOUND = 4,    ///< No such asset.
        BAD_NAME = 5,     ///< Fails AssetStore::IsValidName() for the kind.
        DENIED = 6,       ///< The kind does not allow the operation.
        IN_USE = 7,       ///< The provider's is_in_use refuses the operation.
        TOO_LARGE = 8,    ///< `len` above the kind's max, or data beyond `len`.
        NO_SPACE = 9,     ///< The volume has no room for `len`, or filled up.
        BAD_OFFSET = 10,  ///< Not the next offset (the error carries `off`), or past the end (it carries `len`).
        BAD_TOKEN = 11,   ///< The token, kind or name does not match the open upload.
        BUSY = 12,        ///< Another client has an upload open.
        CRC_MISMATCH = 13,
        IO_ERROR = 14,
        NO_MEMORY = 15,
    };

    /// Directory entries read for one LIST request: more than one response holds.
    static constexpr size_t MAX_PAGE_ENTRIES = 48;

private:
    struct Job {
        Command command;
        smp_streamer* ctxt;
        int rc = MGMT_ERR_EUNKNOWN;
    };

    struct ReadSession {
        // The provider the file was opened through: a kind registered anew is another session.
        std::shared_ptr<const AssetProvider> provider;
        AssetKind kind{};
        std::string name;
        // Open while the session is.
        AssetReader reader;
        // CRC32 of the file's bytes [0, crc_end).
        uint32_t crc = 0;
        size_t crc_end = 0;
        int64_t last_activity_ms = 0;
    };

    struct UploadSession {
        AssetKind kind{};
        std::string name;
        // 0 while no upload is open, which is when `writer` is not open either.
        uint32_t token = 0;
        // What the first chunk declared: the size of the file and the CRC32 of all of it.
        size_t length = 0;
        uint32_t declared_crc = 0;
        // CRC32 of what was received.
        uint32_t crc = 0;
        size_t received = 0;
        // Of the first chunk, to tell a first chunk sent again from another client's.
        size_t first_length = 0;
        uint32_t first_crc = 0;
        AssetWriter writer;
        int64_t last_activity_ms = 0;
    };

    // The upload that was stored last, to answer its last chunk again when the answer got lost.
    struct LastCommit {
        // 0 when there is none.
        uint32_t token = 0;
        AssetKind kind{};
        std::string name;
        // What the first chunk declared.
        size_t length = 0;
        uint32_t declared_crc = 0;
        // Of the last chunk: where it starts, how long it is, and its CRC32.
        size_t offset = 0;
        size_t chunk_length = 0;
        uint32_t chunk_crc = 0;
        int64_t time_ms = 0;
    };

    // What the last closed read session had computed, for the file at the time.
    struct Checkpoint {
        bool is_valid = false;
        AssetKind kind{};
        std::string name;
        size_t size = 0;
        uint32_t crc = 0;
        size_t crc_end = 0;
    };

    static const mgmt_handler HANDLERS[];

    std::shared_ptr<AssetsManager> assets_manager_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::optional<WorkQueueTask<AssetsMgmtGroup>> job_task_;
    std::optional<WorkQueueTask<AssetsMgmtGroup>> idle_task_;
    // The job a handler waits for; cleared by the job as it finishes, before the handler wakes.
    Job* job_ = nullptr;
    k_sem job_done_{};
    // Set while the group is destroyed: the idle task is not armed again after its last cancel.
    std::atomic<bool> is_stopping_{ false };

    // Chunks pass through it on their way between a file and a response; as large as a response.
    std::pmr::vector<uint8_t> transfer_buffer_;
    ReadSession read_session_;
    Checkpoint checkpoint_;
    UploadSession upload_;
    LastCommit last_commit_;

    int HandleKinds(smp_streamer* ctxt);
    int HandleList(smp_streamer* ctxt);
    int HandleRead(smp_streamer* ctxt);
    int HandleWrite(smp_streamer* ctxt);
    int HandleDelete(smp_streamer* ctxt);

    int RunJob(Command command, smp_streamer* ctxt);
    WorkQueueTaskResult ProcessJob();
    int ExecuteJob(const Job& job);
    WorkQueueTaskResult ProcessIdle();

    int ExecuteKinds(smp_streamer* ctxt);
    int ExecuteList(smp_streamer* ctxt);
    int ExecuteRead(smp_streamer* ctxt);
    int ExecuteWrite(smp_streamer* ctxt);
    int ExecuteDelete(smp_streamer* ctxt);

    int WriteFirstChunk(
        smp_streamer* ctxt,
        uint32_t kind,
        std::string_view name,
        uint32_t length,
        uint32_t crc,
        bool has_token,
        uint32_t token,
        std::span<const uint8_t> chunk
    );
    int WriteNextChunk(
        smp_streamer* ctxt,
        uint32_t kind,
        std::string_view name,
        uint32_t offset,
        bool has_token,
        uint32_t token,
        std::span<const uint8_t> chunk
    );
    /**
     * @brief Appends the chunk that comes next to the open upload, and stores the file when it is the
     *        last. The upload is aborted whenever this fails, or throws.
     */
    int AcceptChunk(
        smp_streamer* ctxt,
        const std::shared_ptr<const AssetProvider>& provider,
        uint32_t offset,
        std::span<const uint8_t> chunk,
        uint32_t chunk_crc
    );
    /** @brief Checks and stores the upload that has all its data; ends it either way. */
    int FinishUpload(
        smp_streamer* ctxt,
        const AssetProvider& provider,
        uint32_t offset,
        size_t chunk_length,
        uint32_t chunk_crc
    );
    static int RespondToWrite(smp_streamer* ctxt, size_t offset, uint32_t token);

    /** @param extra_key Sent with @p extra_value next to the error, unless empty. */
    static int Fail(smp_streamer* ctxt, Error error, std::string_view extra_key = {}, uint32_t extra_value = 0);
    /** @brief Maps a negative errno of the asset store to a group error. */
    static Error ToError(int rc);

    std::shared_ptr<const AssetProvider> FindProvider(uint32_t kind) const;
    /**
     * @brief Resolves the provider of @p kind and checks that it allows @p access on @p name.
     * @return OK, or UNKNOWN_KIND, DENIED, BAD_NAME or UNAVAILABLE, in that order. @p provider is
     *         set whenever the kind is known.
     */
    Error CheckTarget(
        uint32_t kind,
        std::string_view name,
        AssetAccess access,
        std::shared_ptr<const AssetProvider>& provider
    ) const;

    /**
     * @brief Whether the open read session serves @p name through @p provider, exactly as spelled:
     *        the one a chunk past the first can be read from.
     */
    bool IsReadSessionOf(const std::shared_ptr<const AssetProvider>& provider, std::string_view name) const;
    /**
     * @brief Whether the open read session may be on the file of @p kind and @p name, whichever
     *        provider it was opened through. Names compare ignoring ASCII case, as FAT does: a
     *        session that is closed without need only costs a reopen, an open file that is deleted
     *        corrupts a FAT volume.
     */
    bool MayBeReadSessionOf(AssetKind kind, std::string_view name) const;
    /**
     * @brief Opens the read session on @p name, closing the one that is open.
     * @param use_checkpoint Start the CRC from the checkpoint of this file, when there is one.
     * @throws Whatever the provider's is_in_use throws, with no session open.
     */
    Error
    OpenReadSession(const std::shared_ptr<const AssetProvider>& provider, std::string_view name, bool use_checkpoint);
    /** @brief Reads [crc_end, @p offset) of the open file into the running CRC. */
    Error ExtendReadCrc(size_t offset);
    /** @brief Closes the read session, if open, and keeps its CRC as the checkpoint. Never throws. */
    void CloseReadSession();
    /** @brief Forgets what a session knew; its reader is closed already. */
    void ResetReadSession();
    /** @brief Forgets the checkpoint if it may be of the file of @p kind and @p name (ASCII case ignored). */
    void InvalidateCheckpoint(AssetKind kind, std::string_view name);

    /** @brief Whether the upload is the one @p token, @p kind and @p name name. */
    bool IsUploadOf(uint32_t kind, std::string_view name, bool has_token, uint32_t token) const;
    /** @brief Whether a first chunk is the open upload's own, sent again. */
    bool IsFirstChunkRepeat(
        uint32_t kind,
        std::string_view name,
        size_t length,
        uint32_t crc,
        size_t chunk_length,
        uint32_t chunk_crc
    ) const;
    /**
     * @brief Whether a first chunk, sent without a token, is the file of one chunk that was stored
     *        last, sent again in time, and the file is there still.
     */
    bool IsStoredFirstChunkRepeat(
        const AssetProvider& provider,
        std::string_view name,
        size_t length,
        uint32_t crc,
        size_t chunk_length,
        uint32_t chunk_crc
    ) const;
    /** @brief Whether a chunk is the last chunk of the upload that was stored last, sent again in time. */
    bool IsLastChunkRepeat(
        uint32_t kind,
        std::string_view name,
        bool has_token,
        uint32_t token,
        uint32_t offset,
        size_t chunk_length,
        uint32_t chunk_crc
    ) const;
    /** @brief Forgets the upload stored last if it may be of the file of @p kind and @p name (ASCII case ignored). */
    void InvalidateLastCommit(AssetKind kind, std::string_view name);
    /** @brief Deletes the staged file of the open upload, if any, and forgets the upload. */
    void AbortUpload();
    /** @brief Forgets the upload; its writer is closed already. */
    void ResetUpload();
    /** @brief Aborts the upload if it was idle for the session timeout. */
    void ExpireUpload(int64_t now_ms);
    /** @return The time until the first open session expires; INT64_MAX when none is open. */
    int64_t GetIdleDelayMs(int64_t now_ms) const;
    static Error ToWriteError(int rc);

    void OnActivity();

    static int TranslateError(uint16_t error);

public:
    /**
     * @param assets_manager The registry of the kinds to serve; may be empty, and kinds may come
     *        and go while the group is served.
     * @param config_work_queue_thread Runs the requests, as it runs the other configuration work.
     */
    AssetsMgmtGroup(
        std::shared_ptr<AssetsManager> assets_manager,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread
    );
    ~AssetsMgmtGroup();

    /**
     * @brief Creates the work queue tasks and the transfer buffer, and deletes staged files a power
     *        loss left on the registered kinds' volumes. Needs a running work queue. Call before
     *        Register().
     */
    bool Initialize();
};

} // namespace eerie_leap::subsys::assets::smp
