#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <new>
#include <utility>

#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/random/rng.h"
#include "subsys/threading/scoped_mutex.h"

#include "config_mgmt_group.h"

LOG_MODULE_REGISTER(config_mgmt_group, LOG_LEVEL_INF);

namespace eerie_leap::domain::configuration_domain::smp {

using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::random::Rng;
using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;
using eerie_leap::subsys::threading::ScopedMutex;

using Type = ConfigurationService::Type;

namespace {

constexpr int64_t SESSION_TIMEOUT_MS = CONFIG_EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS;

// Room kept in a read response for the keys around the data.
constexpr size_t READ_RESPONSE_OVERHEAD = 48;

// More than there are configuration types.
constexpr size_t MAX_TYPES = 16;

void Release(std::pmr::vector<uint8_t>& buffer) {
    std::pmr::vector<uint8_t> empty(buffer.get_allocator());
    buffer.swap(empty);
}

uint32_t Crc(std::span<const uint8_t> data) {
    return crc32_ieee(data.data(), data.size());
}

} // namespace

const mgmt_handler ConfigMgmtGroup::HANDLERS[] = {
    {.mh_read = Dispatch<&ConfigMgmtGroup::HandleList>, .mh_write = nullptr},
    {.mh_read = Dispatch<&ConfigMgmtGroup::HandleCrc>, .mh_write = nullptr},
    {.mh_read = Dispatch<&ConfigMgmtGroup::HandleRead>, .mh_write = nullptr},
    {.mh_read = nullptr, .mh_write = Dispatch<&ConfigMgmtGroup::HandleWrite>},
};

ConfigMgmtGroup::ConfigMgmtGroup(
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread)
        : SmpGroup(SmpGroupId::CONFIG, HANDLERS, TranslateError),
        configuration_service_(std::move(configuration_service)),
        work_queue_thread_(std::move(config_work_queue_thread)),
        session_{.data = std::pmr::vector<uint8_t>(Mrm::GetExtPmr())},
        snapshot_{.data = std::pmr::vector<uint8_t>(Mrm::GetExtPmr())} {

    k_mutex_init(&lock_);
    k_sem_init(&job_done_, 0, 1);
}

ConfigMgmtGroup::~ConfigMgmtGroup() {
    Unregister();

    if(idle_task_.has_value())
        idle_task_->Cancel();

    if(job_task_.has_value())
        job_task_->Cancel();
}

bool ConfigMgmtGroup::Initialize() {
    if(job_task_.has_value())
        return true;

    try {
        job_task_ = work_queue_thread_->CreateTask(
            [](ConfigMgmtGroup* group) { return group->ProcessJob(); }, this);
        idle_task_ = work_queue_thread_->CreateTask(
            [](ConfigMgmtGroup* group) { return group->ProcessIdle(); }, this);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the config group tasks: %s", e.what());
        job_task_.reset();
        idle_task_.reset();

        return false;
    }

    return true;
}

int ConfigMgmtGroup::HandleList(smp_streamer* ctxt) {
    std::array<Type, MAX_TYPES> types{};
    const size_t count = configuration_service_->GetRegisteredTypes(types);

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "types") && zcbor_list_start_encode(zse, count);

    for(size_t i = 0; ok && i < count; i++) {
        const auto max_size = static_cast<uint32_t>(configuration_service_->GetMaxCborSize(types[i]));

        ok = zcbor_map_start_encode(zse, 2)
            && zcbor_tstr_put_lit(zse, "type") && zcbor_uint32_put(zse, std::to_underlying(types[i]))
            && zcbor_tstr_put_lit(zse, "max") && zcbor_uint32_put(zse, max_size)
            && zcbor_map_end_encode(zse, 2);
    }

    ok = ok && zcbor_list_end_encode(zse, count);

    return MGMT_RETURN_CHECK(ok);
}

int ConfigMgmtGroup::HandleCrc(smp_streamer* ctxt) {
    uint32_t type = 0;
    std::array keys = {SmpMapKey("type", zcbor_uint32_decode, &type)};

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    if(!IsKnownType(type))
        return Fail(ctxt, Error::UNKNOWN_TYPE);

    const auto info = configuration_service_->GetCborConfigurationInfo(static_cast<Type>(type));

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = zcbor_tstr_put_lit(zse, "type") && zcbor_uint32_put(zse, type)
        && zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, static_cast<uint32_t>(info.size))
        && zcbor_tstr_put_lit(zse, "crc") && zcbor_uint32_put(zse, info.crc)
        && zcbor_tstr_put_lit(zse, "applied") && zcbor_bool_put(zse, info.is_applied);

    return MGMT_RETURN_CHECK(ok);
}

int ConfigMgmtGroup::HandleRead(smp_streamer* ctxt) {
    uint32_t type = 0;
    uint32_t offset = 0;
    std::array keys = {
        SmpMapKey("type", zcbor_uint32_decode, &type),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    if(!IsKnownType(type))
        return Fail(ctxt, Error::UNKNOWN_TYPE);

    const auto config_type = static_cast<Type>(type);

    // Every later chunk comes from the snapshot taken here.
    if(offset == 0) {
        Job job{.kind = JobKind::SNAPSHOT, .type = config_type};
        const Error error = RunJob(job);
        if(error != Error::OK)
            return Fail(ctxt, error);
    }

    ScopedMutex guard(lock_);

    if(!snapshot_.is_valid || snapshot_.type != config_type || offset > snapshot_.data.size())
        return Fail(ctxt, Error::BAD_OFFSET);

    zcbor_state_t* zse = ctxt->writer->zs;
    const size_t available = static_cast<size_t>(zse->payload_end - zse->payload);
    const size_t remaining = snapshot_.data.size() - offset;
    const size_t chunk = available > READ_RESPONSE_OVERHEAD
        ? std::min(remaining, available - READ_RESPONSE_OVERHEAD)
        : 0;

    if(chunk == 0 && remaining > 0)
        return MGMT_ERR_EMSGSIZE;

    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, offset);

    if(ok && offset == 0) {
        ok = zcbor_tstr_put_lit(zse, "len") && zcbor_uint32_put(zse, static_cast<uint32_t>(snapshot_.data.size()))
            && zcbor_tstr_put_lit(zse, "crc") && zcbor_uint32_put(zse, snapshot_.crc);
    }

    const char* chunk_data = chunk > 0 ? reinterpret_cast<const char*>(snapshot_.data.data() + offset) : "";
    ok = ok && zcbor_tstr_put_lit(zse, "data") && zcbor_bstr_encode_ptr(zse, chunk_data, chunk);

    if(offset + chunk == snapshot_.data.size()) {
        ReleaseSnapshotLocked();
    } else {
        snapshot_.last_activity_ms = k_uptime_get();
        OnActivity();
    }

    return MGMT_RETURN_CHECK(ok);
}

int ConfigMgmtGroup::HandleWrite(smp_streamer* ctxt) {
    uint32_t type = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
    uint32_t token = 0;
    zcbor_string data{};
    std::array keys = {
        SmpMapKey("type", zcbor_uint32_decode, &type),
        SmpMapKey("off", zcbor_uint32_decode, &offset),
        SmpMapKey("data", zcbor_bstr_decode, &data),
        SmpMapKey("len", zcbor_uint32_decode, &length),
        SmpMapKey("crc", zcbor_uint32_decode, &crc),
        SmpMapKey("tok", zcbor_uint32_decode, &token),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || !keys[2].found)
        return MGMT_ERR_EINVAL;

    if(offset == 0 && (!keys[3].found || !keys[4].found))
        return MGMT_ERR_EINVAL;

    const bool has_token = keys[5].found;

    if(!IsKnownType(type))
        return Fail(ctxt, Error::UNKNOWN_TYPE);

    const auto config_type = static_cast<Type>(type);

    if(offset == 0) {
        if(length > configuration_service_->GetMaxCborSize(config_type))
            return Fail(ctxt, Error::TOO_LARGE);

        {
            ScopedMutex guard(lock_);

            // The session owner may restart its transfer by presenting its token.
            const bool is_open = session_.token != 0
                && k_uptime_get() - session_.last_activity_ms < SESSION_TIMEOUT_MS;
            if(is_open && (!has_token || token != session_.token)) {
                // A client whose answer got lost sends its first request again, and gets that answer.
                const bool is_repeat = session_.type == config_type
                    && session_.data.size() == length
                    && session_.crc == crc
                    && session_.received == data.len
                    && std::equal(data.value, data.value + data.len, session_.data.begin());
                if(!is_repeat)
                    return Fail(ctxt, Error::BUSY);

                session_.last_activity_ms = k_uptime_get();
                OnActivity();

                return RespondToWrite(ctxt, session_.received, session_.token);
            }
        }

        Job job{.kind = JobKind::STAGE, .type = config_type, .size = length, .crc = crc};
        const Error error = RunJob(job);
        if(error != Error::OK)
            return Fail(ctxt, error);
    }

    uint32_t session_token = 0;
    size_t received = 0;
    bool is_complete = false;

    {
        ScopedMutex guard(lock_);

        if(session_.token != 0 && k_uptime_get() - session_.last_activity_ms >= SESSION_TIMEOUT_MS)
            CloseSessionLocked();

        if(session_.token == 0 || session_.type != config_type
            || (offset != 0 && (!has_token || token != session_.token))) {

            return Fail(ctxt, Error::BAD_TOKEN);
        }

        // A client whose response got lost learns where to resume.
        if(offset != session_.received) {
            zcbor_state_t* zse = ctxt->writer->zs;
            const bool ok = smp_add_cmd_err(zse, std::to_underlying(SmpGroupId::CONFIG), std::to_underlying(Error::BAD_OFFSET))
                && zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, static_cast<uint32_t>(session_.received));

            return MGMT_RETURN_CHECK(ok);
        }

        if(data.len > session_.data.size() - session_.received) {
            CloseSessionLocked();
            return Fail(ctxt, Error::TOO_LARGE);
        }

        std::copy_n(data.value, data.len, session_.data.begin() + session_.received);
        session_.received += data.len;
        session_.last_activity_ms = k_uptime_get();

        session_token = session_.token;
        received = session_.received;
        is_complete = session_.received == session_.data.size();
    }

    if(is_complete) {
        Job job{.kind = JobKind::APPLY, .type = config_type};
        const Error error = RunJob(job);
        if(error != Error::OK)
            return Fail(ctxt, error, error == Error::APPLY_FAILED ? apply_reason_.data() : "");
    } else {
        OnActivity();
    }

    return RespondToWrite(ctxt, received, offset == 0 ? session_token : 0);
}

int ConfigMgmtGroup::Fail(smp_streamer* ctxt, Error error, std::string_view message) {
    if(message.empty())
        return SmpAddGroupError(ctxt, SmpGroupId::CONFIG, std::to_underlying(error));

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = smp_add_cmd_err(zse, std::to_underlying(SmpGroupId::CONFIG), std::to_underlying(error))
        && zcbor_tstr_put_lit(zse, "msg") && zcbor_tstr_encode_ptr(zse, message.data(), message.size());

    return MGMT_RETURN_CHECK(ok);
}

int ConfigMgmtGroup::RespondToWrite(smp_streamer* ctxt, size_t received, uint32_t token) {
    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, static_cast<uint32_t>(received));
    if(ok && token != 0)
        ok = zcbor_tstr_put_lit(zse, "tok") && zcbor_uint32_put(zse, token);

    return MGMT_RETURN_CHECK(ok);
}

bool ConfigMgmtGroup::IsKnownType(uint32_t type) const {
    return type <= UINT8_MAX && configuration_service_->IsRegistered(static_cast<Type>(type));
}

void ConfigMgmtGroup::OnActivity() {
    idle_task_->Reschedule(K_MSEC(SESSION_TIMEOUT_MS));
}

ConfigMgmtGroup::Error ConfigMgmtGroup::RunJob(Job& job) {
    job_ = &job;
    job_task_->Schedule();
    k_sem_take(&job_done_, K_FOREVER);
    job_ = nullptr;

    return job.result;
}

WorkQueueTaskResult ConfigMgmtGroup::ProcessJob() {
    Job* job = job_;
    if(job == nullptr)
        return {};

    try {
        job->result = ExecuteJob(*job);
    } catch(const std::bad_alloc&) {
        job->result = Error::NO_MEMORY;
    } catch(const std::exception& e) {
        LOG_ERR("Config group job failed: %s", e.what());
        job->result = Error::UNKNOWN;
    }

    k_sem_give(&job_done_);

    return {};
}

ConfigMgmtGroup::Error ConfigMgmtGroup::ExecuteJob(Job& job) {
    switch(job.kind) {
        case JobKind::STAGE: {
            ScopedMutex guard(lock_);

            CloseSessionLocked();
            session_.data.resize(job.size);

            uint32_t token = 0;
            while(token == 0)
                token = Rng::Get<uint32_t>();

            session_.type = job.type;
            session_.token = token;
            session_.crc = job.crc;
            session_.received = 0;
            session_.last_activity_ms = k_uptime_get();

            return Error::OK;
        }

        case JobKind::APPLY: {
            ScopedMutex guard(lock_);

            // The session may have timed out while the request was queued.
            if(session_.token == 0 || session_.received != session_.data.size())
                return Error::BAD_TOKEN;

            apply_reason_.front() = '\0';

            Error result = Error::OK;
            if(Crc(session_.data) != session_.crc)
                result = Error::CRC_MISMATCH;
            else if(!configuration_service_->ApplyCborConfiguration(session_.type, session_.data, apply_reason_))
                result = Error::APPLY_FAILED;

            CloseSessionLocked();

            return result;
        }

        case JobKind::SNAPSHOT: {
            auto data = configuration_service_->GetCborConfiguration(job.type);

            ScopedMutex guard(lock_);

            ReleaseSnapshotLocked();
            snapshot_.data = std::move(data);
            snapshot_.type = job.type;
            snapshot_.crc = Crc(snapshot_.data);
            snapshot_.is_valid = true;
            snapshot_.last_activity_ms = k_uptime_get();

            return Error::OK;
        }
    }

    return Error::UNKNOWN;
}

WorkQueueTaskResult ConfigMgmtGroup::ProcessIdle() {
    ScopedMutex guard(lock_);

    const int64_t now_ms = k_uptime_get();
    int64_t next_check_ms = INT64_MAX;

    if(session_.token != 0) {
        const int64_t idle_ms = now_ms - session_.last_activity_ms;
        if(idle_ms >= SESSION_TIMEOUT_MS) {
            LOG_WRN("Config write session timed out after %u of %u bytes.",
                static_cast<unsigned>(session_.received), static_cast<unsigned>(session_.data.size()));
            CloseSessionLocked();
        } else {
            next_check_ms = SESSION_TIMEOUT_MS - idle_ms;
        }
    }

    if(snapshot_.is_valid) {
        const int64_t idle_ms = now_ms - snapshot_.last_activity_ms;
        if(idle_ms >= SESSION_TIMEOUT_MS)
            ReleaseSnapshotLocked();
        else
            next_check_ms = std::min(next_check_ms, SESSION_TIMEOUT_MS - idle_ms);
    }

    if(next_check_ms == INT64_MAX)
        return {};

    return {.reschedule = true, .delay = K_MSEC(next_check_ms)};
}

void ConfigMgmtGroup::CloseSessionLocked() {
    session_.token = 0;
    session_.received = 0;
    Release(session_.data);
}

void ConfigMgmtGroup::ReleaseSnapshotLocked() {
    snapshot_.is_valid = false;
    Release(snapshot_.data);
}

int ConfigMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::UNKNOWN_TYPE:
            return MGMT_ERR_ENOENT;
        case Error::TOO_LARGE:
        case Error::BAD_OFFSET:
        case Error::BAD_TOKEN:
            return MGMT_ERR_EINVAL;
        case Error::BUSY:
            return MGMT_ERR_EBUSY;
        case Error::CRC_MISMATCH:
            return MGMT_ERR_ECORRUPT;
        case Error::NO_MEMORY:
            return MGMT_ERR_ENOMEM;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::configuration_domain::smp
