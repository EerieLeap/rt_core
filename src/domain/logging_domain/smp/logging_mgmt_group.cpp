#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

#include "logging_mgmt_group.h"

namespace eerie_leap::domain::logging_domain::smp {

using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

const mgmt_handler LoggingMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&LoggingMgmtGroup::HandleStatus>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&LoggingMgmtGroup::HandleStart> },
    { .mh_read = nullptr, .mh_write = Dispatch<&LoggingMgmtGroup::HandleStop> },
};

LoggingMgmtGroup::LoggingMgmtGroup(std::shared_ptr<ILoggingControl> control, LogSessionProvider session_provider) :
    SmpGroup(SmpGroupId::LOGGING, HANDLERS, TranslateError),
    control_(std::move(control)),
    session_provider_(std::move(session_provider)) {
    if(control_ == nullptr)
        throw std::invalid_argument("The logging control is required.");
}

LoggingMgmtGroup::~LoggingMgmtGroup() {
    Unregister();
}

int LoggingMgmtGroup::TranslateError(uint16_t error) {
    return error == 0 ? MGMT_ERR_EOK : MGMT_ERR_EUNKNOWN;
}

bool LoggingMgmtGroup::DecodeUid(smp_streamer* ctxt, uint32_t& uid) {
    uid = ILoggingControl::ALL_UNITS;
    std::array keys = { SmpMapKey("uid", zcbor_uint32_decode, &uid) };

    // A missing uid stays ALL_UNITS; a value of another type or beyond 32 bits fails the decode.
    return SmpDecodeRequest(ctxt, keys);
}

int LoggingMgmtGroup::HandleStatus(smp_streamer* ctxt) {
    const bool is_active = control_->IsActive();
    const auto reason = is_active ? 0U : std::to_underlying(control_->GetReason());
    const auto session = session_provider_ ? session_provider_() : std::nullopt;

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "active")
        && zcbor_bool_put(zse, is_active)
        && zcbor_tstr_put_lit(zse, "reason")
        && zcbor_uint32_put(zse, reason);

    if(ok && session.has_value()) {
        ok = zcbor_tstr_put_lit(zse, "since_ms")
            && zcbor_uint64_put(zse, session->since_ms > 0 ? static_cast<uint64_t>(session->since_ms) : 0);

        const size_t name_length = strnlen(session->file_name.data(), session->file_name.size());
        if(ok && name_length > 0) {
            ok = zcbor_tstr_put_lit(zse, "file")
                && zcbor_tstr_encode_ptr(zse, session->file_name.data(), name_length)
                && zcbor_tstr_put_lit(zse, "bytes")
                && zcbor_uint64_put(zse, session->file_bytes);
        }
    }

    return MGMT_RETURN_CHECK(ok);
}

int LoggingMgmtGroup::HandleStart(smp_streamer* ctxt) {
    uint32_t uid = 0;
    if(!DecodeUid(ctxt, uid))
        return MGMT_ERR_EINVAL;

    control_->RequestStart(uid);

    return MGMT_ERR_EOK;
}

int LoggingMgmtGroup::HandleStop(smp_streamer* ctxt) {
    uint32_t uid = 0;
    if(!DecodeUid(ctxt, uid))
        return MGMT_ERR_EINVAL;

    control_->RequestStop(uid);

    return MGMT_ERR_EOK;
}

} // namespace eerie_leap::domain::logging_domain::smp
