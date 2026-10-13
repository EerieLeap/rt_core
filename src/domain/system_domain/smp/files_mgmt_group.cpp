#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>

#include <zephyr/logging/log.h>

#include "files_mgmt_group.h"

LOG_MODULE_REGISTER(files_mgmt_group, LOG_LEVEL_INF);

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

namespace {

// An entry's map and keys, and the ends of the list and of the response map, encode to less than
// this plus the length of the name.
constexpr size_t ENTRY_OVERHEAD = 32;

} // namespace

const mgmt_handler FilesMgmtGroup::HANDLERS[] = {
    {.mh_read = Dispatch<&FilesMgmtGroup::HandleList>, .mh_write = nullptr},
};

FilesMgmtGroup::FilesMgmtGroup(
    std::shared_ptr<const IFsService> sd_fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread)
        : SmpGroup(SmpGroupId::FILES, HANDLERS, TranslateError),
        sd_fs_service_(std::move(sd_fs_service)),
        work_queue_thread_(std::move(config_work_queue_thread)) {

    k_sem_init(&job_done_, 0, 1);
}

FilesMgmtGroup::~FilesMgmtGroup() {
    Unregister();

    if(job_task_.has_value())
        job_task_->Cancel();
}

bool FilesMgmtGroup::Initialize() {
    if(job_task_.has_value())
        return true;

    try {
        job_task_ = work_queue_thread_->CreateTask(
            [](FilesMgmtGroup* group) { return group->ProcessJob(); }, this);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the files group task: %s", e.what());
        job_task_.reset();

        return false;
    }

    return true;
}

int FilesMgmtGroup::HandleList(smp_streamer* ctxt) {
    zcbor_string directory{};
    uint32_t offset = 0;
    std::array keys = {
        SmpMapKey("dir", zcbor_tstr_decode, &directory),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
    };

    // An empty request lists the card's root from its first entry.
    const zcbor_state_t* zsd = ctxt->reader->zs;
    if(zsd->payload != zsd->payload_end && !SmpDecodeRequest(ctxt, keys))
        return MGMT_ERR_EINVAL;

    if(sd_fs_service_ == nullptr || !sd_fs_service_->IsAvailable())
        return Fail(ctxt, Error::NO_CARD);

    const std::string_view path(reinterpret_cast<const char*>(directory.value), directory.len);
    if(IsBadPath(path))
        return Fail(ctxt, Error::BAD_PATH);

    job_directory_ = path;
    job_offset_ = offset;
    job_task_->Schedule();
    k_sem_take(&job_done_, K_FOREVER);
    job_directory_ = {};

    if(job_result_ != Error::OK) {
        ReleasePage();
        return Fail(ctxt, job_result_);
    }

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "total") && zcbor_uint32_put(zse, static_cast<uint32_t>(count_))
        && zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, offset)
        && zcbor_tstr_put_lit(zse, "entries") && zcbor_list_start_encode(zse, page_.size());

    size_t listed = 0;
    for(const auto& entry : page_) {
        if(!ok || static_cast<size_t>(zse->payload_end - zse->payload) < ENTRY_OVERHEAD + entry.name.size())
            break;

        const auto size = static_cast<uint32_t>(std::min<size_t>(entry.size, UINT32_MAX));
        ok = zcbor_map_start_encode(zse, 3)
            && zcbor_tstr_put_lit(zse, "name") && zcbor_tstr_encode_ptr(zse, entry.name.data(), entry.name.size())
            && zcbor_tstr_put_lit(zse, "size") && zcbor_uint32_put(zse, size)
            && zcbor_tstr_put_lit(zse, "type") && zcbor_uint32_put(zse, entry.is_directory ? 1 : 0)
            && zcbor_map_end_encode(zse, 3);
        listed++;
    }

    ok = ok && zcbor_list_end_encode(zse, page_.size());

    // A page without its first entry would read as the end of the directory.
    const bool fits_none = listed == 0 && !page_.empty();
    ReleasePage();

    if(fits_none)
        return MGMT_ERR_EMSGSIZE;

    return MGMT_RETURN_CHECK(ok);
}

int FilesMgmtGroup::Fail(smp_streamer* ctxt, Error error) {
    return SmpAddGroupError(ctxt, SmpGroupId::FILES, std::to_underlying(error));
}

bool FilesMgmtGroup::IsBadPath(std::string_view directory) {
    // FAT separates with a backslash too, which the ".." check would miss; a NUL would cut the
    // path short.
    return directory.size() > MAX_DIR_LENGTH
        || directory.starts_with('/')
        || directory.find('\\') != std::string_view::npos
        || directory.find('\0') != std::string_view::npos;
}

void FilesMgmtGroup::ReleasePage() {
    std::vector<DirectoryEntry>().swap(page_);
}

WorkQueueTaskResult FilesMgmtGroup::ProcessJob() {
    try {
        job_result_ = ToError(sd_fs_service_->ListDirectory(
            job_directory_, job_offset_, MAX_PAGE_ENTRIES, page_, count_));
    } catch(const std::bad_alloc&) {
        job_result_ = Error::NO_MEMORY;
    } catch(const std::exception& e) {
        LOG_ERR("Files group job failed: %s", e.what());
        job_result_ = Error::UNKNOWN;
    }

    k_sem_give(&job_done_);

    return {};
}

FilesMgmtGroup::Error FilesMgmtGroup::ToError(int rc) {
    switch(rc) {
        case 0:
            return Error::OK;
        case -ENODEV:
            return Error::NO_CARD;
        case -ENOENT:
            return Error::NOT_FOUND;
        case -EINVAL:
        case -ENOTDIR:
            return Error::BAD_PATH;
        case -ENOMEM:
            return Error::NO_MEMORY;
        default:
            return Error::IO_ERROR;
    }
}

int FilesMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::NO_CARD:
            return MGMT_ERR_EBADSTATE;
        case Error::NOT_FOUND:
            return MGMT_ERR_ENOENT;
        case Error::BAD_PATH:
            return MGMT_ERR_EINVAL;
        case Error::NO_MEMORY:
            return MGMT_ERR_ENOMEM;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::system_domain::smp
