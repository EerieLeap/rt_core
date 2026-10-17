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

#include "utilities/memory/memory_resource_manager.h"

#include "assets_mgmt_group.h"

LOG_MODULE_REGISTER(assets_mgmt_group, LOG_LEVEL_INF);

namespace eerie_leap::subsys::assets::smp {

using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

namespace {

constexpr int64_t SESSION_TIMEOUT_MS = CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS;

// Worst-case encodings, in bytes, of what the responses are made of.
// A text key shorter than 24 bytes: its head byte and its characters.
constexpr size_t KeySize(size_t length) { return 1 + length; }
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
static_assert(READ_RESPONSE_OVERHEAD >= 3 * (KeySize(3) + MAX_UINT_SIZE)
    + KeySize(4) + MAX_STRING_HEAD_SIZE + MAP_END_SIZE);

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
    return std::ranges::equal(a, b, [](char x, char y) { return ToLowerAscii(x) == ToLowerAscii(y); });
}

std::string_view ToView(const zcbor_string& text) {
    return {reinterpret_cast<const char*>(text.value), text.len};
}

// The room the encoder has left.
size_t Remaining(const zcbor_state_t* zse) {
    return static_cast<size_t>(zse->payload_end - zse->payload);
}

} // namespace

const mgmt_handler AssetsMgmtGroup::HANDLERS[] = {
    {.mh_read = Dispatch<&AssetsMgmtGroup::HandleKinds>, .mh_write = nullptr},
    {.mh_read = Dispatch<&AssetsMgmtGroup::HandleList>, .mh_write = nullptr},
    {.mh_read = Dispatch<&AssetsMgmtGroup::HandleRead>, .mh_write = nullptr},
    // WRITE is not served yet: MCUmgr answers ENOTSUP.
    {.mh_read = nullptr, .mh_write = nullptr},
    {.mh_read = nullptr, .mh_write = Dispatch<&AssetsMgmtGroup::HandleDelete>},
};

AssetsMgmtGroup::AssetsMgmtGroup(
    std::shared_ptr<AssetsManager> assets_manager,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread)
        : SmpGroup(SmpGroupId::ASSETS, HANDLERS, TranslateError),
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

    if(idle_task_.has_value())
        idle_task_->Cancel();

    // The open file, if any, closes with the read session.
}

bool AssetsMgmtGroup::Initialize() {
    if(job_task_.has_value())
        return true;

    try {
        transfer_buffer_.resize(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE);

        job_task_ = work_queue_thread_->CreateTask(
            [](AssetsMgmtGroup* group) { return group->ProcessJob(); }, this);
        idle_task_ = work_queue_thread_->CreateTask(
            [](AssetsMgmtGroup* group) { return group->ProcessIdle(); }, this);

        // A staged upload is deleted by the next one, but holds its space until then.
        for(const auto& provider : assets_manager_->GetProviders()) {
            if(provider->store->IsAvailable() && !provider->store->RemoveStaged()) {
                LOG_WRN("Failed to delete the staged upload of asset kind %u.",
                    static_cast<unsigned>(std::to_underlying(provider->kind)));
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

int AssetsMgmtGroup::HandleDelete(smp_streamer* ctxt) {
    return RunJob(Command::DELETE, ctxt);
}

int AssetsMgmtGroup::RunJob(Command command, smp_streamer* ctxt) {
    Job job{.command = command, .ctxt = ctxt};

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
        case Command::DELETE:
            return ExecuteDelete(job.ctxt);
        case Command::WRITE:
            break;
    }

    return MGMT_ERR_ENOTSUP;
}

WorkQueueTaskResult AssetsMgmtGroup::ProcessIdle() {
    if(!read_session_.reader.IsOpen())
        return {};

    const int64_t idle_ms = k_uptime_get() - read_session_.last_activity_ms;
    if(idle_ms < SESSION_TIMEOUT_MS)
        return {.reschedule = true, .delay = K_MSEC(SESSION_TIMEOUT_MS - idle_ms)};

    LOG_INF("Assets read session timed out at byte %u.", static_cast<unsigned>(read_session_.crc_end));
    CloseReadSession();

    return {};
}

int AssetsMgmtGroup::ExecuteKinds(smp_streamer* ctxt) {
    // Can throw, so it comes before anything is encoded.
    const auto providers = assets_manager_->GetProviders();

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "kinds") && zcbor_list_start_encode(zse, providers.size());

    for(const auto& provider : providers) {
        const AssetStore& store = *provider->store;
        const std::string& directory = store.GetDirectory();

        ok = ok && zcbor_map_start_encode(zse, 7)
            && zcbor_tstr_put_lit(zse, "kind") && zcbor_uint32_put(zse, std::to_underlying(provider->kind))
            && zcbor_tstr_put_lit(zse, "name") && zcbor_tstr_encode_ptr(zse, provider->name.data(), provider->name.size())
            && zcbor_tstr_put_lit(zse, "dir") && zcbor_tstr_encode_ptr(zse, directory.data(), directory.size())
            && zcbor_tstr_put_lit(zse, "access") && zcbor_uint32_put(zse, provider->access)
            && zcbor_tstr_put_lit(zse, "max") && zcbor_uint32_put(zse, provider->max_size)
            && zcbor_tstr_put_lit(zse, "ext") && zcbor_list_start_encode(zse, provider->extensions.size());

        for(const std::string_view extension : provider->extensions)
            ok = ok && zcbor_tstr_encode_ptr(zse, extension.data(), extension.size());

        ok = ok && zcbor_list_end_encode(zse, provider->extensions.size())
            && zcbor_tstr_put_lit(zse, "avail") && zcbor_bool_put(zse, store.IsAvailable())
            && zcbor_map_end_encode(zse, 7);
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
            && zcbor_tstr_put_lit(zse, "name") && zcbor_tstr_encode_ptr(zse, entry.name.data(), entry.name.size())
            && zcbor_tstr_put_lit(zse, "size") && zcbor_uint32_put(zse, size)
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
    const size_t chunk = std::min({size - offset, room, transfer_buffer_.size()});

    if(chunk == 0 && size > offset)
        return MGMT_ERR_EMSGSIZE;

    if(chunk > 0) {
        const ssize_t read = read_session_.reader.Read(offset, std::span(transfer_buffer_).first(chunk));
        if(read != static_cast<ssize_t>(chunk)) {
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

    if(!store.Delete(asset_name))
        return Fail(ctxt, Error::IO_ERROR);

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

AssetsMgmtGroup::Error AssetsMgmtGroup::CheckTarget(uint32_t kind, std::string_view name, AssetAccess access,
    std::shared_ptr<const AssetProvider>& provider) const {

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

bool AssetsMgmtGroup::IsReadSessionOf(const std::shared_ptr<const AssetProvider>& provider, std::string_view name) const {
    return read_session_.reader.IsOpen() && read_session_.provider == provider && read_session_.name == name;
}

bool AssetsMgmtGroup::MayBeReadSessionOf(AssetKind kind, std::string_view name) const {
    return read_session_.reader.IsOpen() && read_session_.kind == kind && EqualsIgnoreCase(read_session_.name, name);
}

AssetsMgmtGroup::Error AssetsMgmtGroup::OpenReadSession(
    const std::shared_ptr<const AssetProvider>& provider, std::string_view name, bool use_checkpoint) {

    CloseReadSession();

    // A download from the start does not trust what was kept of the file.
    if(!use_checkpoint)
        InvalidateCheckpoint(provider->kind, name);

    // The session is open once its reader is, so the reader opens last.
    read_session_.kind = provider->kind;
    read_session_.name.assign(name);

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
    if(use_checkpoint && checkpoint_.is_valid && checkpoint_.kind == provider->kind && checkpoint_.size == size
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
        const ssize_t read = read_session_.reader.Read(read_session_.crc_end, std::span(transfer_buffer_).first(length));

        // Fewer bytes than the file had when it was opened: it was cut short.
        if(read != static_cast<ssize_t>(length))
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
            checkpoint_.crc = read_session_.crc;
            checkpoint_.crc_end = read_session_.crc_end;
            checkpoint_.is_valid = true;
        } catch(const std::bad_alloc&) {
            // Without a checkpoint a resume reads the file again, which only costs time.
        }
    }

    ResetReadSession();
}

void AssetsMgmtGroup::ResetReadSession() {
    // The provider is not kept past the session: what it captured may be waiting to go.
    read_session_.provider.reset();
    read_session_.kind = {};
    read_session_.name.clear();
    read_session_.crc = 0;
    read_session_.crc_end = 0;
    read_session_.last_activity_ms = 0;
}

void AssetsMgmtGroup::InvalidateCheckpoint(AssetKind kind, std::string_view name) {
    if(checkpoint_.is_valid && checkpoint_.kind == kind && EqualsIgnoreCase(checkpoint_.name, name))
        checkpoint_.is_valid = false;
}

void AssetsMgmtGroup::OnActivity() {
    if(is_stopping_)
        return;

    idle_task_->Reschedule(K_MSEC(SESSION_TIMEOUT_MS));
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
