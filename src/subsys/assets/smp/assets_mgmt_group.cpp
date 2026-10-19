#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <new>
#include <span>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "subsys/random/rng.h"
#include "utilities/memory/memory_resource_manager.h"

#include "assets_mgmt_group.h"

LOG_MODULE_REGISTER(assets_mgmt_group, LOG_LEVEL_INF);

namespace eerie_leap::subsys::assets::smp {

using eerie_leap::subsys::random::Rng;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;
using eerie_leap::utilities::memory::Mrm;

namespace {

constexpr int64_t SESSION_TIMEOUT_MS = CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS;
constexpr size_t STAT_CRC_MAX_SIZE = CONFIG_EERIE_LEAP_SMP_ASSETS_STAT_CRC_MAX_SIZE;

// Worst-case encodings, in bytes, of what the responses are made of.
// A text key shorter than 24 bytes: its head byte and its characters.
constexpr size_t KeySize(size_t length) {
    return 1 + length;
}
// An unsigned integer of up to 32 bits: a head byte and four value bytes.
constexpr size_t MAX_UINT_SIZE = 5;
// The head of a byte string of up to 4 GiB.
constexpr size_t MAX_STRING_HEAD_SIZE = 5;
// The break that ends the response map, written by MCUmgr after the handler returns.
constexpr size_t MAP_END_SIZE = 1;

// Room kept in a read response for everything around the data: `off`, `len` and `crc` (each a key
// and an unsigned integer), the key and head of `data`, and the end of the map. That is 38 bytes
// at most; the rest is margin, as in the config group.
constexpr size_t READ_RESPONSE_OVERHEAD = 48;
static_assert(
    READ_RESPONSE_OVERHEAD >= 3 * (KeySize(3) + MAX_UINT_SIZE) + KeySize(4) + MAX_STRING_HEAD_SIZE + MAP_END_SIZE
);

// Names are shorter than 256 bytes, so their head is two bytes at most.
static_assert(CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH < 256);

// Room an entry of a list needs besides its name: the map, the `name` and `size` keys, the name's
// head, a 32-bit size and the end of the map. That is 19 bytes at most.
constexpr size_t LIST_ENTRY_OVERHEAD = 24;
static_assert(LIST_ENTRY_OVERHEAD >= 1 + KeySize(4) + 2 + KeySize(4) + MAX_UINT_SIZE + MAP_END_SIZE);

// Room kept behind the last entry that is listed: the end of the list, `next` and the end of the
// response map. That is 12 bytes at most.
constexpr size_t LIST_TAIL_SIZE = 16;
static_assert(LIST_TAIL_SIZE >= 1 + KeySize(4) + MAX_UINT_SIZE + MAP_END_SIZE);

constexpr char ToLowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return ToLowerAscii(x) == ToLowerAscii(y);
    });
}

std::string_view ToView(const zcbor_string& text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor hands out the text as raw bytes
    return { reinterpret_cast<const char*>(text.value), text.len };
}

// The room the encoder has left.
size_t Remaining(const zcbor_state_t* zse) {
    return static_cast<size_t>(zse->payload_end - zse->payload);
}

} // namespace

const mgmt_handler AssetsMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&AssetsMgmtGroup::HandleKinds>, .mh_write = nullptr },
    { .mh_read = Dispatch<&AssetsMgmtGroup::HandleList>, .mh_write = nullptr },
    { .mh_read = Dispatch<&AssetsMgmtGroup::HandleRead>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&AssetsMgmtGroup::HandleWrite> },
    { .mh_read = nullptr, .mh_write = Dispatch<&AssetsMgmtGroup::HandleDelete> },
    { .mh_read = Dispatch<&AssetsMgmtGroup::HandleStat>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&AssetsMgmtGroup::HandleRename> },
};

AssetsMgmtGroup::AssetsMgmtGroup(
    std::shared_ptr<AssetsManager> assets_manager,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread
) :
    SmpGroup(SmpGroupId::ASSETS, HANDLERS, TranslateError),
    assets_manager_(std::move(assets_manager)),
    work_queue_thread_(std::move(config_work_queue_thread)),
    transfer_buffer_(Mrm::GetExtPmr()) {

    k_sem_init(&job_done_, 0, 1);
}

AssetsMgmtGroup::~AssetsMgmtGroup() {
    Unregister();

    // A job arms the idle task (OnActivity()). The job task is cancelled first, which waits for a job
    // that is running, and nothing arms the idle task after that or before its cancel.
    is_stopping_ = true;

    if(job_task_.has_value())
        job_task_->Cancel();

    // A handler whose job was cancelled before it ran would wait for ever.
    if(job_ != nullptr) {
        job_->rc = MGMT_ERR_EBADSTATE;
        job_ = nullptr;
        k_sem_give(&job_done_);
    }

    // The idle task re-arms itself when it runs: a run that began before is_stopping_ was set may
    // do it after the first cancel (a reschedule with a delay ignores a cancel that is under way), so
    // it is cancelled again, and a run after that finds the flag.
    if(idle_task_.has_value()) {
        idle_task_->Cancel();
        idle_task_->Cancel();
    }

    // Nothing runs on the group any more. An upload that is open leaves no staged file behind; the
    // file of a download closes with the read session.
    AbortUpload();
}

bool AssetsMgmtGroup::Initialize() {
    if(job_task_.has_value())
        return true;

    try {
        transfer_buffer_.resize(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE);

        job_task_ = work_queue_thread_->CreateTask(
            [](AssetsMgmtGroup* group) {
                return group->ProcessJob();
            },
            this
        );
        idle_task_ = work_queue_thread_->CreateTask(
            [](AssetsMgmtGroup* group) {
                return group->ProcessIdle();
            },
            this
        );

        // A staged upload is deleted by the next one, but holds its space until then.
        for(const auto& provider : assets_manager_->GetProviders()) {
            if(provider->store->IsAvailable() && !provider->store->RemoveStaged()) {
                LOG_WRN(
                    "Failed to delete the staged upload of asset kind %u.",
                    static_cast<unsigned>(std::to_underlying(provider->kind))
                );
            }
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to initialize the assets group: %s", e.what());
        job_task_.reset();
        idle_task_.reset();
        std::pmr::vector<uint8_t>(Mrm::GetExtPmr()).swap(transfer_buffer_);

        return false;
    }

    return true;
}

int AssetsMgmtGroup::HandleKinds(smp_streamer* ctxt) {
    return RunJob(Command::KINDS, ctxt);
}

int AssetsMgmtGroup::HandleList(smp_streamer* ctxt) {
    return RunJob(Command::LIST, ctxt);
}

int AssetsMgmtGroup::HandleRead(smp_streamer* ctxt) {
    return RunJob(Command::READ, ctxt);
}

int AssetsMgmtGroup::HandleWrite(smp_streamer* ctxt) {
    return RunJob(Command::WRITE, ctxt);
}

int AssetsMgmtGroup::HandleDelete(smp_streamer* ctxt) {
    return RunJob(Command::DELETE, ctxt);
}

int AssetsMgmtGroup::HandleStat(smp_streamer* ctxt) {
    return RunJob(Command::STAT, ctxt);
}

int AssetsMgmtGroup::HandleRename(smp_streamer* ctxt) {
    return RunJob(Command::RENAME, ctxt);
}

int AssetsMgmtGroup::RunJob(Command command, smp_streamer* ctxt) {
    // Initialize() creates the task; without it no job could ever run and the wait below would not end.
    if(!job_task_.has_value())
        return Fail(ctxt, Error::UNKNOWN);

    Job job{ .command = command, .ctxt = ctxt };

    job_ = &job;
    job_task_->Schedule();
    k_sem_take(&job_done_, K_FOREVER);

    // The job cleared job_ before it woke this handler, which has only its own Job to read.
    return job.rc;
}

WorkQueueTaskResult AssetsMgmtGroup::ProcessJob() {
    Job* job = job_;
    if(job == nullptr)
        return {};

    // A job encodes its response only after everything that can throw, so the error below is never
    // appended to a half-written response.
    try {
        job->rc = ExecuteJob(*job);
    } catch(const std::bad_alloc&) {
        job->rc = Fail(job->ctxt, Error::NO_MEMORY);
    } catch(const std::exception& e) {
        LOG_ERR("Assets group job failed: %s", e.what());
        job->rc = Fail(job->ctxt, Error::UNKNOWN);
    } catch(...) {
        LOG_ERR("Assets group job failed.");
        job->rc = Fail(job->ctxt, Error::UNKNOWN);
    }

    // The handler wakes to its own Job and nothing of the group's: it may be gone after the give.
    job_ = nullptr;
    k_sem_give(&job_done_);

    return {};
}

int AssetsMgmtGroup::ExecuteJob(const Job& job) {
    switch(job.command) {
        case Command::KINDS:
            return ExecuteKinds(job.ctxt);
        case Command::LIST:
            return ExecuteList(job.ctxt);
        case Command::READ:
            return ExecuteRead(job.ctxt);
        case Command::WRITE:
            return ExecuteWrite(job.ctxt);
        case Command::DELETE:
            return ExecuteDelete(job.ctxt);
        case Command::STAT:
            return ExecuteStat(job.ctxt);
        case Command::RENAME:
            return ExecuteRename(job.ctxt);
    }

    return MGMT_ERR_ENOTSUP;
}

WorkQueueTaskResult AssetsMgmtGroup::ProcessIdle() {
    // Not armed again once the group is destroyed.
    if(is_stopping_)
        return {};

    const int64_t now_ms = k_uptime_get();

    if(read_session_.reader.IsOpen() && now_ms - read_session_.last_activity_ms >= SESSION_TIMEOUT_MS) {
        LOG_INF("Assets read session timed out at byte %u.", static_cast<unsigned>(read_session_.crc_end));
        CloseReadSession();
    }

    ExpireUpload(now_ms);

    // Whichever session expires first.
    const int64_t delay_ms = GetIdleDelayMs(now_ms);
    if(delay_ms == INT64_MAX)
        return {};

    return { .reschedule = true, .delay = K_MSEC(delay_ms) };
}

int AssetsMgmtGroup::ExecuteKinds(smp_streamer* ctxt) {
    // Can throw, so it comes before anything is encoded.
    const auto providers = assets_manager_->GetProviders();

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "kinds") && zcbor_list_start_encode(zse, providers.size());

    for(const auto& provider : providers) {
        const AssetStore& store = *provider->store;
        const std::string& directory = store.GetDirectory();
        const bool is_available = store.IsAvailable();
        const AssetStore::Space space = is_available ? store.GetSpace() : AssetStore::Space{};
        // free and used join the entry while the volume is mounted.
        const size_t entry_size = is_available ? 9 : 7;

        ok = ok
            && zcbor_map_start_encode(zse, entry_size)
            && zcbor_tstr_put_lit(zse, "kind")
            && zcbor_uint32_put(zse, std::to_underlying(provider->kind))
            && zcbor_tstr_put_lit(zse, "name")
            && zcbor_tstr_encode_ptr(zse, provider->name.data(), provider->name.size())
            && zcbor_tstr_put_lit(zse, "dir")
            && zcbor_tstr_encode_ptr(zse, directory.data(), directory.size())
            && zcbor_tstr_put_lit(zse, "access")
            && zcbor_uint32_put(zse, provider->access)
            && zcbor_tstr_put_lit(zse, "max")
            && zcbor_uint32_put(zse, provider->max_size)
            && zcbor_tstr_put_lit(zse, "ext")
            && zcbor_list_start_encode(zse, provider->extensions.size());

        for(const std::string_view extension : provider->extensions)
            ok = ok && zcbor_tstr_encode_ptr(zse, extension.data(), extension.size());

        ok = ok
            && zcbor_list_end_encode(zse, provider->extensions.size())
            && zcbor_tstr_put_lit(zse, "avail")
            && zcbor_bool_put(zse, is_available);

        if(is_available) {
            ok = ok
                && zcbor_tstr_put_lit(zse, "free")
                && zcbor_uint64_put(zse, space.free)
                && zcbor_tstr_put_lit(zse, "used")
                && zcbor_uint64_put(zse, space.used);
        }

        ok = ok && zcbor_map_end_encode(zse, entry_size);
    }

    ok = ok && zcbor_list_end_encode(zse, providers.size());

    return MGMT_RETURN_CHECK(ok);
}

int AssetsMgmtGroup::ExecuteList(smp_streamer* ctxt) {
    uint32_t kind = 0;
    uint32_t offset = 0;
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    const auto provider = FindProvider(kind);
    if(provider == nullptr)
        return Fail(ctxt, Error::UNKNOWN_KIND);

    std::vector<AssetEntry> entries;
    size_t scanned = 0;
    size_t count = 0;

    const int rc = provider->store->List(offset, MAX_PAGE_ENTRIES, provider->extensions, entries, scanned, count);
    if(rc == -ENODEV)
        return Fail(ctxt, Error::UNAVAILABLE);
    if(rc != 0)
        return Fail(ctxt, rc == -ENOMEM ? Error::NO_MEMORY : Error::IO_ERROR);

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "entries") && zcbor_list_start_encode(zse, entries.size());

    size_t listed = 0;
    while(ok && listed < entries.size()) {
        const AssetEntry& entry = entries[listed];
        if(Remaining(zse) < LIST_ENTRY_OVERHEAD + entry.name.size() + LIST_TAIL_SIZE)
            break;

        const auto size = static_cast<uint32_t>(std::min<size_t>(entry.size, UINT32_MAX));
        ok = zcbor_map_start_encode(zse, 2)
            && zcbor_tstr_put_lit(zse, "name")
            && zcbor_tstr_encode_ptr(zse, entry.name.data(), entry.name.size())
            && zcbor_tstr_put_lit(zse, "size")
            && zcbor_uint32_put(zse, size)
            && zcbor_map_end_encode(zse, 2);
        listed++;
    }

    ok = ok && zcbor_list_end_encode(zse, entries.size());

    // The directory position to ask for next: the first entry that did not fit, else the end of what
    // was read, unless that is the end of the directory.
    std::optional<uint32_t> next;
    if(listed < entries.size()) {
        // An entry that does not fit even alone would send the client round in circles.
        if(listed == 0)
            return MGMT_ERR_EMSGSIZE;

        next = static_cast<uint32_t>(entries[listed].index);
    } else if(scanned > 0 && offset + scanned < count) {
        next = static_cast<uint32_t>(offset + scanned);
    }

    if(ok && next.has_value())
        ok = zcbor_tstr_put_lit(zse, "next") && zcbor_uint32_put(zse, *next);

    return MGMT_RETURN_CHECK(ok);
}

int AssetsMgmtGroup::ExecuteRead(smp_streamer* ctxt) {
    uint32_t kind = 0;
    uint32_t offset = 0;
    zcbor_string name{};
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("name", zcbor_tstr_decode, &name),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || !keys[2].found)
        return MGMT_ERR_EINVAL;

    const std::string_view asset_name = ToView(name);
    std::shared_ptr<const AssetProvider> provider;

    Error error = CheckTarget(kind, asset_name, AssetAccess::READ, provider);

    // An open file must not outlive its volume, and every file of the kind is on it.
    if(error == Error::UNAVAILABLE && read_session_.reader.IsOpen() && read_session_.kind == provider->kind)
        CloseReadSession();

    if(error != Error::OK)
        return Fail(ctxt, error);

    if(offset == 0 || !IsReadSessionOf(provider, asset_name)) {
        // A download that starts over is not continued from what an earlier one knew of the file: it
        // may have changed since, at the same size, and a CRC that is never recomputed would stay.
        error = OpenReadSession(provider, asset_name, offset > 0);
        if(error != Error::OK)
            return Fail(ctxt, error);
    }

    const size_t size = read_session_.reader.GetSize();

    // The session stays: the client may be asking for a chunk it has not got yet.
    if(offset > size)
        return Fail(ctxt, Error::BAD_OFFSET, "len", static_cast<uint32_t>(size));

    // A resume after a reboot, or after a request that got lost.
    error = ExtendReadCrc(offset);
    if(error != Error::OK) {
        CloseReadSession();

        return Fail(ctxt, error);
    }

    zcbor_state_t* zse = ctxt->writer->zs;
    const size_t available = Remaining(zse);
    const size_t room = available > READ_RESPONSE_OVERHEAD ? available - READ_RESPONSE_OVERHEAD : 0;
    const size_t chunk = std::min({ size - offset, room, transfer_buffer_.size() });

    if(chunk == 0 && size > offset)
        return MGMT_ERR_EMSGSIZE;

    if(chunk > 0) {
        const ssize_t read = read_session_.reader.Read(offset, std::span(transfer_buffer_).first(chunk));
        if(std::cmp_not_equal(read, chunk)) {
            CloseReadSession();

            return Fail(ctxt, Error::IO_ERROR);
        }

        // The CRC is at or past the offset by now. A chunk below its end is a repeat.
        if(offset + chunk > read_session_.crc_end) {
            const size_t known = read_session_.crc_end - offset;

            read_session_.crc = crc32_ieee_update(read_session_.crc, transfer_buffer_.data() + known, chunk - known);
            read_session_.crc_end = offset + chunk;
        }
    }

    const bool is_last = offset + chunk == size;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the chunk is raw file bytes, zcbor takes char*
    const char* chunk_data = chunk > 0 ? reinterpret_cast<const char*>(transfer_buffer_.data()) : "";

    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, offset);

    if(ok && offset == 0)
        ok = zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, static_cast<uint32_t>(size));

    ok = ok && zcbor_tstr_put_lit(zse, "data") && zcbor_bstr_encode_ptr(zse, chunk_data, chunk);

    if(ok && is_last)
        ok = zcbor_tstr_put_lit(zse, "crc") && zcbor_uint32_put(zse, read_session_.crc);

    if(is_last) {
        CloseReadSession();
    } else {
        read_session_.last_activity_ms = k_uptime_get();
        OnActivity();
    }

    return MGMT_RETURN_CHECK(ok);
}

int AssetsMgmtGroup::ExecuteStat(smp_streamer* ctxt) {
    uint32_t kind = 0;
    zcbor_string name{};
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("name", zcbor_tstr_decode, &name),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    const std::string_view asset_name = ToView(name);
    std::shared_ptr<const AssetProvider> provider;

    const Error error = CheckTarget(kind, asset_name, AssetAccess::READ, provider);
    if(error != Error::OK)
        return Fail(ctxt, error);

    const AssetStore& store = *provider->store;

    // Read before the size: a write in between makes the result stale at worst.
    const uint64_t generation = store.GetGeneration();

    const std::optional<size_t> stored_size = store.GetSize(asset_name);
    if(!stored_size.has_value())
        return Fail(ctxt, Error::NOT_FOUND);

    // May throw; no reader is open yet.
    if(provider->is_in_use && provider->is_in_use(AssetAccess::READ, asset_name))
        return Fail(ctxt, Error::IN_USE);

    size_t size = *stored_size;
    std::optional<uint32_t> crc;

    if(IsReadSessionOf(provider, asset_name)
        && read_session_.generation == generation
        && read_session_.crc_end == size
        && read_session_.reader.GetSize() == size) {

        // The open download has read all of it.
        crc = read_session_.crc;
    } else if(checkpoint_.is_valid
        && checkpoint_.kind == provider->kind
        && checkpoint_.size == size
        && checkpoint_.crc_end == size
        && checkpoint_.generation == generation
        && checkpoint_.name == asset_name) {

        crc = checkpoint_.crc;
    } else if(size <= STAT_CRC_MAX_SIZE) {
        // The read session is left as it is: this reader is its own, closed when it goes out of scope.
        AssetReader reader;

        const int rc = store.OpenRead(asset_name, reader);
        if(rc < 0)
            return Fail(ctxt, ToError(rc));

        // What was opened is what is summed, whatever the file was a moment ago.
        size = reader.GetSize();

        if(size > UINT32_MAX)
            return Fail(ctxt, Error::IO_ERROR);

        if(size <= STAT_CRC_MAX_SIZE) {
            uint32_t sum = 0;
            size_t done = 0;

            while(done < size) {
                const size_t length = std::min(size - done, transfer_buffer_.size());
                const ssize_t read = reader.Read(done, std::span(transfer_buffer_).first(length));
                if(std::cmp_not_equal(read, length))
                    return Fail(ctxt, Error::IO_ERROR);

                sum = crc32_ieee_update(sum, transfer_buffer_.data(), length);
                done += length;
            }

            crc = sum;

            // The next STAT of this file costs nothing, as long as it does not change.
            checkpoint_.is_valid = false;

            try {
                checkpoint_.name.assign(asset_name);
                checkpoint_.kind = provider->kind;
                checkpoint_.size = size;
                checkpoint_.generation = generation;
                checkpoint_.crc = sum;
                checkpoint_.crc_end = size;
                checkpoint_.is_valid = true;
            } catch(const std::bad_alloc&) {
                LOG_WRN("Out of memory recording the STAT checkpoint.");
            }
        }
    }

    if(size > UINT32_MAX)
        return Fail(ctxt, Error::IO_ERROR);

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, static_cast<uint32_t>(size));

    if(ok && crc.has_value())
        ok = zcbor_tstr_put_lit(zse, "crc") && zcbor_uint32_put(zse, *crc);

    return MGMT_RETURN_CHECK(ok);
}

int AssetsMgmtGroup::ExecuteWrite(smp_streamer* ctxt) {
    uint32_t kind = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    uint32_t token = 0;
    zcbor_string name{};
    zcbor_string data{};
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("name", zcbor_tstr_decode, &name),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
        SmpMapKey("data", zcbor_bstr_decode, &data),
        SmpMapKey("len", zcbor_uint32_decode, &length),
        SmpMapKey("crc", zcbor_uint32_decode, &crc),
        SmpMapKey("tok", zcbor_uint32_decode, &token),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || !keys[2].found || !keys[3].found)
        return MGMT_ERR_EINVAL;

    if(offset == 0 && (!keys[4].found || !keys[5].found))
        return MGMT_ERR_EINVAL;

    // An upload that its client left is no upload by now, whoever asks.
    ExpireUpload(k_uptime_get());

    const std::string_view asset_name = ToView(name);
    const std::span<const uint8_t> chunk(data.value, data.len);
    const bool has_token = keys[6].found;

    if(offset == 0)
        return WriteFirstChunk(ctxt, kind, asset_name, length, crc, has_token, token, chunk);

    return WriteNextChunk(ctxt, kind, asset_name, offset, has_token, token, chunk);
}

int AssetsMgmtGroup::WriteFirstChunk(
    smp_streamer* ctxt,
    uint32_t kind,
    std::string_view name,
    uint32_t length,
    uint32_t crc,
    bool has_token,
    uint32_t token,
    std::span<const uint8_t> chunk
) {

    std::shared_ptr<const AssetProvider> provider;

    const Error error = CheckTarget(kind, name, AssetAccess::WRITE, provider);
    if(error != Error::OK)
        return Fail(ctxt, error);

    if(length > provider->max_size || chunk.size() > length)
        return Fail(ctxt, Error::TOO_LARGE);

    const uint32_t chunk_crc = crc32_ieee(chunk.data(), chunk.size());

    // The answer to a file of one chunk got lost: it is stored, and the client learns so, whatever
    // else is going on and whether the file is in use by now.
    if(!has_token && IsStoredFirstChunkRepeat(*provider, name, length, crc, chunk.size(), chunk_crc))
        return RespondToWrite(ctxt, last_commit_.length, last_commit_.token);

    // Asked before an upload that is open is looked at: what this check throws or refuses is no
    // reason to touch another client's upload.
    if(provider->is_in_use && provider->is_in_use(AssetAccess::WRITE, name))
        return Fail(ctxt, Error::IN_USE);

    if(upload_.writer.IsOpen()) {
        if(!has_token || token != upload_.token) {
            // A client whose answer got lost sends its first request again, and gets that answer.
            if(!IsFirstChunkRepeat(kind, name, length, crc, chunk.size(), chunk_crc))
                return Fail(ctxt, Error::BUSY);

            upload_.last_activity_ms = k_uptime_get();
            OnActivity();

            return RespondToWrite(ctxt, upload_.received, upload_.token);
        }

        // The owner starts over. What it sent goes, and so does the room it held: the volume is
        // asked for the room after that.
        AbortUpload();
    }

    // No upload is open, and only one is ever staged: a staged file is one that a card pulled in the
    // middle of an upload, say, left behind. It still counts as used.
    provider->store->RemoveStaged();

    if(length > provider->store->GetFreeSpace())
        return Fail(ctxt, Error::NO_SPACE);

    // Everything that can throw comes before the staged file is opened.
    upload_.kind = provider->kind;
    upload_.name.assign(name);

    const int rc = provider->store->BeginWrite(name, upload_.writer);
    if(rc < 0) {
        ResetUpload();

        return Fail(ctxt, ToError(rc));
    }

    uint32_t new_token = 0;
    while(new_token == 0)
        new_token = Rng::Get<uint32_t>();

    upload_.token = new_token;
    upload_.length = length;
    upload_.declared_crc = crc;
    upload_.crc = 0;
    upload_.received = 0;
    upload_.first_length = chunk.size();
    upload_.first_crc = chunk_crc;
    upload_.last_activity_ms = k_uptime_get();

    return AcceptChunk(ctxt, provider, 0, chunk, chunk_crc);
}

int AssetsMgmtGroup::WriteNextChunk(
    smp_streamer* ctxt,
    uint32_t kind,
    std::string_view name,
    uint32_t offset,
    bool has_token,
    uint32_t token,
    std::span<const uint8_t> chunk
) {

    const uint32_t chunk_crc = crc32_ieee(chunk.data(), chunk.size());

    if(!IsUploadOf(kind, name, has_token, token)) {
        // The answer to the last chunk got lost: the file is stored, and the client learns so.
        if(IsLastChunkRepeat(kind, name, has_token, token, offset, chunk.size(), chunk_crc))
            return RespondToWrite(ctxt, last_commit_.length, 0);

        return Fail(ctxt, Error::BAD_TOKEN);
    }

    // The kind, or its volume, may have gone since the upload began.
    const auto provider = FindProvider(kind);
    if(provider == nullptr) {
        AbortUpload();

        return Fail(ctxt, Error::UNKNOWN_KIND);
    }

    if(!provider->store->IsAvailable()) {
        AbortUpload();

        return Fail(ctxt, Error::UNAVAILABLE);
    }

    // A client whose response got lost learns where to resume.
    if(offset != upload_.received)
        return Fail(ctxt, Error::BAD_OFFSET, "off", static_cast<uint32_t>(upload_.received));

    if(chunk.size() > upload_.length - upload_.received) {
        AbortUpload();

        return Fail(ctxt, Error::TOO_LARGE);
    }

    return AcceptChunk(ctxt, provider, offset, chunk, chunk_crc);
}

int AssetsMgmtGroup::AcceptChunk(
    smp_streamer* ctxt,
    const std::shared_ptr<const AssetProvider>& provider,
    uint32_t offset,
    std::span<const uint8_t> chunk,
    uint32_t chunk_crc
) {

    // The staged file does not outlive an exception either.
    try {
        const int rc = upload_.writer.Append(chunk);
        if(rc != 0) {
            AbortUpload();

            return Fail(ctxt, ToWriteError(rc));
        }

        upload_.crc = crc32_ieee_update(upload_.crc, chunk.data(), chunk.size());
        upload_.received += chunk.size();
        upload_.last_activity_ms = k_uptime_get();

        if(upload_.received < upload_.length) {
            OnActivity();

            return RespondToWrite(ctxt, upload_.received, offset == 0 ? upload_.token : 0);
        }

        return FinishUpload(ctxt, *provider, offset, chunk.size(), chunk_crc);
    } catch(...) {
        AbortUpload();

        throw;
    }
}

int AssetsMgmtGroup::FinishUpload(
    smp_streamer* ctxt,
    const AssetProvider& provider,
    uint32_t offset,
    size_t chunk_length,
    uint32_t chunk_crc
) {

    if(upload_.crc != upload_.declared_crc) {
        AbortUpload();

        return Fail(ctxt, Error::CRC_MISMATCH);
    }

    // The file may have gone into use since the first chunk, and the rename replaces it.
    if(provider.is_in_use && provider.is_in_use(AssetAccess::WRITE, upload_.name)) {
        AbortUpload();

        return Fail(ctxt, Error::IN_USE);
    }

    // FatFs has no open-file locking: a file that is replaced while it is open corrupts the volume.
    // A download of the file goes on with the new one when its next request comes, and the CRC it
    // ends with tells the client that the data it holds is a mix.
    if(MayBeReadSessionOf(upload_.kind, upload_.name))
        CloseReadSession();

    InvalidateCheckpoint(upload_.kind, upload_.name);

    // Closes the writer, and deletes the staged file when it fails.
    const int rc = upload_.writer.Commit();
    if(rc != 0) {
        ResetUpload();

        return Fail(ctxt, ToWriteError(rc));
    }

    const uint32_t token = upload_.token;
    const size_t length = upload_.length;

    // The file is stored: failing to remember it must not fail the answer.
    // Its CRC was checked against the declared one, so the checkpoint covers the whole file. The
    // generation is read after the commit, which has increased it.
    checkpoint_.is_valid = false;

    try {
        checkpoint_.name = upload_.name;
        checkpoint_.kind = upload_.kind;
        checkpoint_.size = length;
        checkpoint_.generation = provider.store->GetGeneration();
        checkpoint_.crc = upload_.declared_crc;
        checkpoint_.crc_end = length;
        checkpoint_.is_valid = true;
    } catch(const std::bad_alloc&) {
        LOG_WRN("Out of memory recording the upload's checkpoint, a STAT will read the file.");
    }

    last_commit_.token = 0;

    try {
        last_commit_.name = upload_.name;
        last_commit_.kind = upload_.kind;
        last_commit_.length = length;
        last_commit_.declared_crc = upload_.declared_crc;
        last_commit_.offset = offset;
        last_commit_.chunk_length = chunk_length;
        last_commit_.chunk_crc = chunk_crc;
        last_commit_.time_ms = k_uptime_get();
        last_commit_.token = token;
    } catch(const std::bad_alloc&) {
        // A last chunk sent again then gets BAD_TOKEN, and the client finds the file with a LIST.
        LOG_WRN("Out of memory recording the commit, a repeated last chunk will be answered with BAD_TOKEN.");
    }

    ResetUpload();

    return RespondToWrite(ctxt, length, offset == 0 ? token : 0);
}

int AssetsMgmtGroup::RespondToWrite(smp_streamer* ctxt, size_t offset, uint32_t token) {
    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, static_cast<uint32_t>(offset));

    if(ok && token != 0)
        ok = zcbor_tstr_put_lit(zse, "tok") && zcbor_uint32_put(zse, token);

    return MGMT_RETURN_CHECK(ok);
}

int AssetsMgmtGroup::ExecuteDelete(smp_streamer* ctxt) {
    uint32_t kind = 0;
    zcbor_string name{};
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("name", zcbor_tstr_decode, &name),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    const std::string_view asset_name = ToView(name);
    std::shared_ptr<const AssetProvider> provider;

    const Error error = CheckTarget(kind, asset_name, AssetAccess::DELETE, provider);
    if(error != Error::OK)
        return Fail(ctxt, error);

    AssetStore& store = *provider->store;

    if(!store.GetSize(asset_name).has_value())
        return Fail(ctxt, Error::NOT_FOUND);

    if(provider->is_in_use && provider->is_in_use(AssetAccess::DELETE, asset_name))
        return Fail(ctxt, Error::IN_USE);

    // FatFs has no open-file locking: deleting an open file corrupts the volume.
    // The names of a FAT volume do not tell upper and lower case apart, and the kind may have been
    // registered anew since the session opened.
    if(MayBeReadSessionOf(provider->kind, asset_name))
        CloseReadSession();

    InvalidateCheckpoint(provider->kind, asset_name);

    // The file is gone: a chunk sent again must not be taken for one that stored it.
    InvalidateLastCommit(provider->kind, asset_name);

    if(!store.Delete(asset_name))
        return Fail(ctxt, Error::IO_ERROR);

    return MGMT_ERR_EOK;
}

int AssetsMgmtGroup::ExecuteRename(smp_streamer* ctxt) {
    uint32_t kind = 0;
    zcbor_string name{};
    zcbor_string target{};
    std::array keys = {
        SmpMapKey("kind", zcbor_uint32_decode, &kind),
        SmpMapKey("name", zcbor_tstr_decode, &name),
        SmpMapKey("to", zcbor_tstr_decode, &target),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || !keys[2].found)
        return MGMT_ERR_EINVAL;

    const std::string_view asset_name = ToView(name);
    const std::string_view to_name = ToView(target);

    // A rename takes a name away and makes one: both bits.
    std::shared_ptr<const AssetProvider> provider = FindProvider(kind);
    if(provider == nullptr)
        return Fail(ctxt, Error::UNKNOWN_KIND);

    if(!HasAccess(provider->access, AssetAccess::WRITE) || !HasAccess(provider->access, AssetAccess::DELETE))
        return Fail(ctxt, Error::DENIED);

    if(!AssetStore::IsValidName(asset_name, provider->extensions)
        || !AssetStore::IsValidName(to_name, provider->extensions)) {

        return Fail(ctxt, Error::BAD_NAME);
    }

    // IFsService::Rename() refuses these, because FAT would delete the file.
    if(EqualsIgnoreCase(asset_name, to_name))
        return Fail(ctxt, Error::BAD_NAME);

    AssetStore& store = *provider->store;

    if(!store.IsAvailable())
        return Fail(ctxt, Error::UNAVAILABLE);

    const std::optional<size_t> size = store.GetSize(asset_name);
    if(!size.has_value())
        return Fail(ctxt, Error::NOT_FOUND);

    if(provider->is_in_use
        && (provider->is_in_use(AssetAccess::DELETE, asset_name) || provider->is_in_use(AssetAccess::WRITE, to_name))) {

        return Fail(ctxt, Error::IN_USE);
    }

    // The staged file of an upload is renamed onto its target by its last chunk: not while this
    // takes the name away or replaces it.
    if(upload_.writer.IsOpen()
        && upload_.kind == provider->kind
        && (EqualsIgnoreCase(upload_.name, asset_name) || EqualsIgnoreCase(upload_.name, to_name))) {

        return Fail(ctxt, Error::BUSY);
    }

    // FatFs has no open-file locking: renaming or replacing an open file corrupts the volume.
    if(MayBeReadSessionOf(provider->kind, asset_name) || MayBeReadSessionOf(provider->kind, to_name))
        CloseReadSession();

    // A chunk sent again must not be taken for one that stored a file that is not there now.
    InvalidateLastCommit(provider->kind, asset_name);
    InvalidateLastCommit(provider->kind, to_name);

    // The checkpoint of the source follows the file if it is current: the bytes are the same.
    const bool follows = checkpoint_.is_valid
        && checkpoint_.kind == provider->kind
        && checkpoint_.size == *size
        && checkpoint_.generation == store.GetGeneration()
        && checkpoint_.name == asset_name;

    const int rc = store.Rename(asset_name, to_name);
    if(rc != 0) {
        InvalidateCheckpoint(provider->kind, asset_name);
        InvalidateCheckpoint(provider->kind, to_name);

        return Fail(ctxt, ToError(rc));
    }

    InvalidateCheckpoint(provider->kind, to_name);

    if(follows) {
        checkpoint_.is_valid = false;

        try {
            checkpoint_.name.assign(to_name);
            checkpoint_.generation = store.GetGeneration();
            checkpoint_.is_valid = true;
        } catch(const std::bad_alloc&) {
            LOG_WRN("Out of memory moving the checkpoint, a STAT will read the file.");
        }
    } else {
        InvalidateCheckpoint(provider->kind, asset_name);
    }

    return MGMT_ERR_EOK;
}

int AssetsMgmtGroup::Fail(smp_streamer* ctxt, Error error, std::string_view extra_key, uint32_t extra_value) {
    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = smp_add_cmd_err(zse, std::to_underlying(SmpGroupId::ASSETS), std::to_underlying(error));

    if(ok && !extra_key.empty())
        ok = zcbor_tstr_encode_ptr(zse, extra_key.data(), extra_key.size()) && zcbor_uint32_put(zse, extra_value);

    return MGMT_RETURN_CHECK(ok);
}

AssetsMgmtGroup::Error AssetsMgmtGroup::ToError(int rc) {
    switch(rc) {
        case 0:
            return Error::OK;
        case -ENODEV:
            return Error::UNAVAILABLE;
        case -ENOENT:
            return Error::NOT_FOUND;
        case -ENOSPC:
            return Error::NO_SPACE;
        case -ENOMEM:
            return Error::NO_MEMORY;
        case -EINVAL:
            return Error::BAD_NAME;
        default:
            return Error::IO_ERROR;
    }
}

std::shared_ptr<const AssetProvider> AssetsMgmtGroup::FindProvider(uint32_t kind) const {
    // The kind is narrowed to the enum's byte: 256 must not read as 0.
    if(kind >= ASSET_KIND_COUNT)
        return nullptr;

    return assets_manager_->Find(static_cast<AssetKind>(kind));
}

AssetsMgmtGroup::Error AssetsMgmtGroup::CheckTarget(
    uint32_t kind,
    std::string_view name,
    AssetAccess access,
    std::shared_ptr<const AssetProvider>& provider
) const {

    provider = FindProvider(kind);
    if(provider == nullptr)
        return Error::UNKNOWN_KIND;

    if(!HasAccess(provider->access, access))
        return Error::DENIED;

    if(!AssetStore::IsValidName(name, provider->extensions))
        return Error::BAD_NAME;

    if(!provider->store->IsAvailable())
        return Error::UNAVAILABLE;

    return Error::OK;
}

bool AssetsMgmtGroup::IsReadSessionOf(
    const std::shared_ptr<const AssetProvider>& provider,
    std::string_view name
) const {
    return read_session_.reader.IsOpen() && read_session_.provider == provider && read_session_.name == name;
}

bool AssetsMgmtGroup::MayBeReadSessionOf(AssetKind kind, std::string_view name) const {
    return read_session_.reader.IsOpen() && read_session_.kind == kind && EqualsIgnoreCase(read_session_.name, name);
}

AssetsMgmtGroup::Error AssetsMgmtGroup::OpenReadSession(
    const std::shared_ptr<const AssetProvider>& provider,
    std::string_view name,
    bool use_checkpoint
) {

    CloseReadSession();

    // A download from the start does not trust what was kept of the file.
    if(!use_checkpoint)
        InvalidateCheckpoint(provider->kind, name);

    // The session is open once its reader is, so the reader opens last. The generation is read
    // first: a write in between only makes a checkpoint of this session stale.
    read_session_.kind = provider->kind;
    read_session_.name.assign(name);
    read_session_.generation = provider->store->GetGeneration();

    const int rc = provider->store->OpenRead(name, read_session_.reader);
    if(rc < 0) {
        ResetReadSession();

        return ToError(rc);
    }

    bool is_in_use = false;

    try {
        is_in_use = provider->is_in_use && provider->is_in_use(AssetAccess::READ, name);
    } catch(...) {
        // No session is left half open, with a handle that nothing closes.
        read_session_.reader.Close();
        ResetReadSession();

        throw;
    }

    if(is_in_use) {
        read_session_.reader.Close();
        ResetReadSession();

        return Error::IN_USE;
    }

    const size_t size = read_session_.reader.GetSize();

    // A size that the protocol cannot express.
    if(size > UINT32_MAX) {
        LOG_WRN("Asset of kind %u is larger than 4 GiB.", static_cast<unsigned>(std::to_underlying(provider->kind)));
        read_session_.reader.Close();
        ResetReadSession();

        return Error::IO_ERROR;
    }

    read_session_.provider = provider;

    // The checkpoint of a session that was closed on this very file (not one that is spelled in
    // another case, which may be another file) continues where it ended.
    if(use_checkpoint
        && checkpoint_.is_valid
        && checkpoint_.kind == provider->kind
        && checkpoint_.size == size
        && checkpoint_.generation == read_session_.generation
        && checkpoint_.name == name) {

        read_session_.crc = checkpoint_.crc;
        read_session_.crc_end = checkpoint_.crc_end;
    }

    read_session_.last_activity_ms = k_uptime_get();
    OnActivity();

    return Error::OK;
}

AssetsMgmtGroup::Error AssetsMgmtGroup::ExtendReadCrc(size_t offset) {
    while(read_session_.crc_end < offset) {
        const size_t length = std::min(offset - read_session_.crc_end, transfer_buffer_.size());
        const ssize_t read = read_session_.reader.Read(
            read_session_.crc_end, std::span(transfer_buffer_).first(length)
        );

        // Fewer bytes than the file had when it was opened: it was cut short.
        if(std::cmp_not_equal(read, length))
            return Error::IO_ERROR;

        read_session_.crc = crc32_ieee_update(read_session_.crc, transfer_buffer_.data(), length);
        read_session_.crc_end += length;
    }

    return Error::OK;
}

void AssetsMgmtGroup::CloseReadSession() {
    if(!read_session_.reader.IsOpen())
        return;

    const size_t size = read_session_.reader.GetSize();
    read_session_.reader.Close();

    // A session that read nothing knows nothing, and must not replace what another one did.
    if(read_session_.crc_end > 0) {
        checkpoint_.is_valid = false;

        try {
            checkpoint_.name = read_session_.name;
            checkpoint_.kind = read_session_.kind;
            checkpoint_.size = size;
            checkpoint_.generation = read_session_.generation;
            checkpoint_.crc = read_session_.crc;
            checkpoint_.crc_end = read_session_.crc_end;
            checkpoint_.is_valid = true;
        } catch(const std::bad_alloc&) {
            // Without a checkpoint a resume reads the file again, which only costs time.
            LOG_WRN("Out of memory recording the read checkpoint, a resume will read the file again.");
        }
    }

    ResetReadSession();
}

void AssetsMgmtGroup::ResetReadSession() {
    // The provider is not kept past the session: what it captured may be waiting to go.
    read_session_.provider.reset();
    read_session_.kind = {};
    read_session_.name.clear();
    read_session_.generation = 0;
    read_session_.crc = 0;
    read_session_.crc_end = 0;
    read_session_.last_activity_ms = 0;
}

void AssetsMgmtGroup::InvalidateCheckpoint(AssetKind kind, std::string_view name) {
    if(checkpoint_.is_valid && checkpoint_.kind == kind && EqualsIgnoreCase(checkpoint_.name, name))
        checkpoint_.is_valid = false;
}

bool AssetsMgmtGroup::IsUploadOf(uint32_t kind, std::string_view name, bool has_token, uint32_t token) const {
    return upload_.writer.IsOpen()
        && has_token
        && token == upload_.token
        && std::to_underlying(upload_.kind) == kind
        && upload_.name == name;
}

bool AssetsMgmtGroup::IsFirstChunkRepeat(
    uint32_t kind,
    std::string_view name,
    size_t length,
    uint32_t crc,
    size_t chunk_length,
    uint32_t chunk_crc
) const {

    // Nothing was accepted after the first chunk.
    return upload_.writer.IsOpen()
        && std::to_underlying(upload_.kind) == kind
        && upload_.name == name
        && upload_.length == length
        && upload_.declared_crc == crc
        && upload_.first_length == chunk_length
        && upload_.first_crc == chunk_crc
        && upload_.received == upload_.first_length;
}

bool AssetsMgmtGroup::IsStoredFirstChunkRepeat(
    const AssetProvider& provider,
    std::string_view name,
    size_t length,
    uint32_t crc,
    size_t chunk_length,
    uint32_t chunk_crc
) const {

    // The upload had one chunk, which is this one, and ended not long ago.
    if(last_commit_.token == 0
        || last_commit_.offset != 0
        || last_commit_.kind != provider.kind
        || last_commit_.name != name
        || last_commit_.length != length
        || last_commit_.declared_crc != crc
        || last_commit_.chunk_length != chunk_length
        || last_commit_.chunk_crc != chunk_crc
        || k_uptime_get() - last_commit_.time_ms >= SESSION_TIMEOUT_MS) {

        return false;
    }

    // And the file is still there as it was stored: not deleted since by someone else, say.
    return provider.store->GetSize(name) == length;
}

bool AssetsMgmtGroup::IsLastChunkRepeat(
    uint32_t kind,
    std::string_view name,
    bool has_token,
    uint32_t token,
    uint32_t offset,
    size_t chunk_length,
    uint32_t chunk_crc
) const {

    return last_commit_.token != 0
        && has_token
        && token == last_commit_.token
        && std::to_underlying(last_commit_.kind) == kind
        && last_commit_.name == name
        && last_commit_.offset == offset
        && last_commit_.chunk_length == chunk_length
        && last_commit_.chunk_crc == chunk_crc
        && k_uptime_get() - last_commit_.time_ms < SESSION_TIMEOUT_MS;
}

void AssetsMgmtGroup::InvalidateLastCommit(AssetKind kind, std::string_view name) {
    if(last_commit_.token != 0 && last_commit_.kind == kind && EqualsIgnoreCase(last_commit_.name, name))
        last_commit_.token = 0;
}

void AssetsMgmtGroup::AbortUpload() {
    // Closes the staged file and deletes it; nothing when no upload is open.
    upload_.writer.Abort();
    ResetUpload();
}

void AssetsMgmtGroup::ResetUpload() {
    upload_.kind = {};
    upload_.name.clear();
    upload_.token = 0;
    upload_.length = 0;
    upload_.declared_crc = 0;
    upload_.crc = 0;
    upload_.received = 0;
    upload_.first_length = 0;
    upload_.first_crc = 0;
    upload_.last_activity_ms = 0;
}

void AssetsMgmtGroup::ExpireUpload(int64_t now_ms) {
    if(!upload_.writer.IsOpen() || now_ms - upload_.last_activity_ms < SESSION_TIMEOUT_MS)
        return;

    LOG_INF(
        "Assets upload timed out at byte %u of %u.", static_cast<unsigned>(upload_.received),
        static_cast<unsigned>(upload_.length)
    );
    AbortUpload();
}

AssetsMgmtGroup::Error AssetsMgmtGroup::ToWriteError(int rc) {
    // FAT reports a full volume as a short write, which AssetWriter takes for -ENOSPC.
    return rc == -ENOSPC ? Error::NO_SPACE : Error::IO_ERROR;
}

int64_t AssetsMgmtGroup::GetIdleDelayMs(int64_t now_ms) const {
    int64_t delay_ms = INT64_MAX;

    if(read_session_.reader.IsOpen()) {
        delay_ms = std::min(
            delay_ms, std::max<int64_t>(0, SESSION_TIMEOUT_MS - (now_ms - read_session_.last_activity_ms))
        );
    }

    if(upload_.writer.IsOpen()) {
        delay_ms = std::min(delay_ms, std::max<int64_t>(0, SESSION_TIMEOUT_MS - (now_ms - upload_.last_activity_ms)));
    }

    return delay_ms;
}

void AssetsMgmtGroup::OnActivity() {
    if(is_stopping_)
        return;

    // Armed for the session that expires first, so a session does not outlive its time by the other's.
    const int64_t delay_ms = GetIdleDelayMs(k_uptime_get());
    if(delay_ms != INT64_MAX && idle_task_.has_value())
        idle_task_->Reschedule(K_MSEC(delay_ms));
}

int AssetsMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::UNKNOWN_KIND:
        case Error::NOT_FOUND:
            return MGMT_ERR_ENOENT;
        case Error::UNAVAILABLE:
            return MGMT_ERR_EBADSTATE;
        case Error::BAD_NAME:
        case Error::BAD_OFFSET:
        case Error::BAD_TOKEN:
        case Error::TOO_LARGE:
            return MGMT_ERR_EINVAL;
        case Error::DENIED:
            return MGMT_ERR_EACCESSDENIED;
        case Error::IN_USE:
        case Error::BUSY:
            return MGMT_ERR_EBUSY;
        case Error::NO_SPACE:
        case Error::NO_MEMORY:
            return MGMT_ERR_ENOMEM;
        case Error::CRC_MISMATCH:
            return MGMT_ERR_ECORRUPT;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::subsys::assets::smp
