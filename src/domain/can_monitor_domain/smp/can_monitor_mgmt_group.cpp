#include "can_monitor_mgmt_group.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace eerie_leap::domain::can_monitor_domain::smp {

using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

using models::CAN_MONITOR_MODE_COUNT;
using models::CanbusType;
using models::CanMonitorMode;
using models::CanMonitorOpenAnswer;
using models::CanMonitorOpenRequest;
using models::CanMonitorPortStatus;
using models::CanMonitorStats;
using models::CanMonitorStatus;
using models::CanSyntheticParameters;

namespace {

// One filter: [id, mask, ext].
bool DecodeFilter(zcbor_state_t* zsd, CanUnitFilter* filter) {
    return zcbor_list_start_decode(zsd)
        && zcbor_uint32_decode(zsd, &filter->id)
        && zcbor_uint32_decode(zsd, &filter->mask)
        && zcbor_bool_decode(zsd, &filter->ext)
        && zcbor_list_end_decode(zsd);
}

bool DecodeFilters(zcbor_state_t* zsd, CanMonitorMgmtGroup::FilterList* list) {
    list->count = 0;

    if(!zcbor_list_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        CanUnitFilter filter;
        if(!DecodeFilter(zsd, &filter))
            return false;

        if(list->count < list->filters.size())
            list->filters[list->count] = filter;
        list->count++;
    }

    return zcbor_list_end_decode(zsd);
}

bool DecodeSynthetic(zcbor_state_t* zsd, CanMonitorMgmtGroup::SyntheticRequest* request) {
    std::array keys = {
        SmpMapKey("fps", zcbor_uint32_decode, &request->fps),
        SmpMapKey("ids", zcbor_uint32_decode, &request->ids),
        SmpMapKey("dlc", zcbor_uint32_decode, &request->dlc),
        SmpMapKey("ext", zcbor_bool_decode, &request->ext),
        SmpMapKey("fd", zcbor_bool_decode, &request->fd),
    };

    size_t decoded = 0;
    if(zcbor_map_decode_bulk(zsd, keys.data(), keys.size(), &decoded) != 0)
        return false;

    request->is_complete = keys[0].found && keys[1].found && keys[2].found;

    return true;
}

} // namespace

const mgmt_handler CanMonitorMgmtGroup::HANDLERS[COMMAND_COUNT] = {
    { .mh_read = Dispatch<&CanMonitorMgmtGroup::HandleStatus>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleOpen> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleClose> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleFilter> },
    { .mh_read = Dispatch<&CanMonitorMgmtGroup::HandleStats>, .mh_write = nullptr },
    // 5 ARM, 6 SEND, 7 PERIODIC, 8 REPLAY (Phase 4).
    { .mh_read = nullptr, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = nullptr },
};

CanMonitorMgmtGroup::CanMonitorMgmtGroup(std::shared_ptr<CanMonitorService> service) :
    SmpGroup(SmpGroupId::CAN_MONITOR, HANDLERS, TranslateError),
    service_(std::move(service)) {}

CanMonitorMgmtGroup::~CanMonitorMgmtGroup() {
    Unregister();
}

int CanMonitorMgmtGroup::HandleOpen(smp_streamer* ctxt) {
    uint32_t channel = 0;
    uint32_t mode = 0;
    uint32_t type = 0;
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    bool is_delta = false;
    SyntheticRequest synthetic;

    std::array keys = {
        SmpMapKey("ch", zcbor_uint32_decode, &channel),
        SmpMapKey("mode", zcbor_uint32_decode, &mode),
        SmpMapKey("type", zcbor_uint32_decode, &type),
        SmpMapKey("bitrate", zcbor_uint32_decode, &bitrate),
        SmpMapKey("data_bitrate", zcbor_uint32_decode, &data_bitrate),
        SmpMapKey("filters", DecodeFilters, &filter_list_),
        SmpMapKey("delta", zcbor_bool_decode, &is_delta),
        SmpMapKey("synthetic", DecodeSynthetic, &synthetic),
    };
    constexpr size_t KEY_CHANNEL = 0;
    constexpr size_t KEY_MODE = 1;
    constexpr size_t KEY_TYPE = 2;
    constexpr size_t KEY_BITRATE = 3;
    constexpr size_t KEY_DATA_BITRATE = 4;
    constexpr size_t KEY_FILTERS = 5;
    constexpr size_t KEY_SYNTHETIC = 7;

    filter_list_.count = 0;

    if(!SmpDecodeRequest(ctxt, keys))
        return MGMT_ERR_EINVAL;

    const bool is_synthetic = keys[KEY_SYNTHETIC].found;

    // `ch` and `mode` are for a session on a bus; a synthetic one has none.
    if(!is_synthetic && (!keys[KEY_CHANNEL].found || !keys[KEY_MODE].found || channel > UINT8_MAX))
        return MGMT_ERR_EINVAL;

    if(is_synthetic && !synthetic.is_complete)
        return MGMT_ERR_EINVAL;

    if(keys[KEY_TYPE].found
        && type != std::to_underlying(CanbusType::CLASSICAL_CAN)
        && type != std::to_underlying(CanbusType::CANFD))
        return MGMT_ERR_EINVAL;

    if(keys[KEY_FILTERS].found && filter_list_.count > filter_list_.filters.size())
        return MGMT_ERR_EINVAL;

    CanMonitorOpenRequest request;
    request.delta = is_delta;
    request.filters = std::span<const CanUnitFilter>(filter_list_.filters).first(filter_list_.count);

    if(keys[KEY_TYPE].found)
        request.type = static_cast<CanbusType>(type);
    if(keys[KEY_BITRATE].found)
        request.bitrate = bitrate;
    if(keys[KEY_DATA_BITRATE].found)
        request.data_bitrate = data_bitrate;

    if(is_synthetic) {
        if(synthetic.dlc > UINT8_MAX)
            return MGMT_ERR_EINVAL;

        const CanSyntheticParameters parameters{ .fps = synthetic.fps,
            .ids = synthetic.ids,
            .dlc = static_cast<uint8_t>(synthetic.dlc),
            .ext = synthetic.ext,
            .fd = synthetic.fd };
        if(!parameters.IsValid())
            return MGMT_ERR_EINVAL;

        request.synthetic = parameters;
    } else {
        request.ch = static_cast<uint8_t>(channel);

        // A mode this firmware doesn't know is a mode the port can't take.
        if(mode >= CAN_MONITOR_MODE_COUNT)
            return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(Error::BAD_MODE));

        request.mode = static_cast<CanMonitorMode>(mode);
    }

    const auto result = service_->Open(request);
    if(!result.has_value())
        return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(result.error()));

    const CanMonitorOpenAnswer& answer = *result;
    zcbor_state_t* zse = ctxt->writer->zs;

    const bool ok = zcbor_tstr_put_lit(zse, "tok")
        && zcbor_uint32_put(zse, answer.tok)
        && zcbor_tstr_put_lit(zse, "ch")
        && zcbor_uint32_put(zse, answer.ch)
        && zcbor_tstr_put_lit(zse, "mode")
        && zcbor_uint32_put(zse, std::to_underlying(answer.mode))
        && zcbor_tstr_put_lit(zse, "type")
        && zcbor_uint32_put(zse, std::to_underlying(answer.type))
        && zcbor_tstr_put_lit(zse, "bitrate")
        && zcbor_uint32_put(zse, answer.bitrate)
        && zcbor_tstr_put_lit(zse, "data_bitrate")
        && zcbor_uint32_put(zse, answer.data_bitrate)
        && zcbor_tstr_put_lit(zse, "lease_ms")
        && zcbor_uint32_put(zse, answer.lease_ms);

    return MGMT_RETURN_CHECK(ok);
}

int CanMonitorMgmtGroup::HandleStatus(smp_streamer* ctxt) {
    const CanMonitorStatus status = service_->GetStatus();
    zcbor_state_t* zse = ctxt->writer->zs;

    bool ok = zcbor_tstr_put_lit(zse, "version")
        && zcbor_uint32_put(zse, models::CAN_MONITOR_VERSION)
        && zcbor_tstr_put_lit(zse, "ports")
        && zcbor_list_start_encode(zse, CanMonitorService::MAX_PORTS);

    for(const CanMonitorPortStatus& port : status.ports) {
        ok = ok
            && zcbor_map_start_encode(zse, 7)
            && zcbor_tstr_put_lit(zse, "ch")
            && zcbor_uint32_put(zse, port.ch)
            && zcbor_tstr_put_lit(zse, "types")
            && zcbor_list_start_encode(zse, 2)
            && zcbor_uint32_put(zse, std::to_underlying(CanbusType::CLASSICAL_CAN))
            && (!port.is_fd_capable || zcbor_uint32_put(zse, std::to_underlying(CanbusType::CANFD)))
            && zcbor_list_end_encode(zse, 2)
            && zcbor_tstr_put_lit(zse, "configured")
            && zcbor_bool_put(zse, port.is_configured)
            && zcbor_tstr_put_lit(zse, "state")
            && zcbor_uint32_put(zse, port.state)
            && zcbor_tstr_put_lit(zse, "bitrate")
            && zcbor_uint32_put(zse, port.bitrate)
            && zcbor_tstr_put_lit(zse, "data_bitrate")
            && zcbor_uint32_put(zse, port.data_bitrate)
            && zcbor_tstr_put_lit(zse, "session")
            && zcbor_bool_put(zse, port.has_session)
            && zcbor_map_end_encode(zse, 7);
    }

    ok = ok
        && zcbor_list_end_encode(zse, CanMonitorService::MAX_PORTS)
        && zcbor_tstr_put_lit(zse, "filters_max")
        && zcbor_uint32_put(zse, status.filters_max)
        && zcbor_tstr_put_lit(zse, "ring_bytes")
        && zcbor_uint32_put(zse, status.ring_bytes)
        && zcbor_tstr_put_lit(zse, "notify_max")
        && zcbor_uint32_put(zse, status.notify_max)
        && zcbor_tstr_put_lit(zse, "tx")
        && zcbor_bool_put(zse, status.has_tx)
        && zcbor_tstr_put_lit(zse, "synthetic")
        && zcbor_bool_put(zse, status.has_synthetic)
        && zcbor_tstr_put_lit(zse, "delta")
        && zcbor_bool_put(zse, status.has_delta);

    return MGMT_RETURN_CHECK(ok);
}

int CanMonitorMgmtGroup::HandleFilter(smp_streamer* ctxt) {
    uint32_t token = 0;
    filter_list_.count = 0;

    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("filters", DecodeFilters, &filter_list_),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    if(filter_list_.count > filter_list_.filters.size())
        return MGMT_ERR_EINVAL;

    const auto result = service_->SetFilters(
        token, std::span<const CanUnitFilter>(filter_list_.filters).first(filter_list_.count)
    );
    if(!result.has_value())
        return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(result.error()));

    return MGMT_ERR_EOK;
}

int CanMonitorMgmtGroup::HandleStats(smp_streamer* ctxt) {
    uint32_t token = 0;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    const auto result = service_->GetStats(token);
    if(!result.has_value())
        return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(result.error()));

    const CanMonitorStats& stats = *result;
    zcbor_state_t* zse = ctxt->writer->zs;

    bool ok = zcbor_tstr_put_lit(zse, "state")
        && zcbor_uint32_put(zse, stats.state)
        && zcbor_tstr_put_lit(zse, "tec")
        && zcbor_uint32_put(zse, stats.tec)
        && zcbor_tstr_put_lit(zse, "rec")
        && zcbor_uint32_put(zse, stats.rec)
        && zcbor_tstr_put_lit(zse, "rx")
        && zcbor_uint64_put(zse, stats.frames_received)
        && zcbor_tstr_put_lit(zse, "tx")
        && zcbor_uint64_put(zse, stats.frames_sent)
        && zcbor_tstr_put_lit(zse, "busy_us")
        && zcbor_uint64_put(zse, stats.busy_us)
        && zcbor_tstr_put_lit(zse, "errors")
        && zcbor_list_start_encode(zse, stats.errors.size());
    for(const uint64_t value : stats.errors)
        ok = ok && zcbor_uint64_put(zse, value);

    ok = ok
        && zcbor_list_end_encode(zse, stats.errors.size())
        && zcbor_tstr_put_lit(zse, "lost")
        && zcbor_list_start_encode(zse, stats.lost.size());
    for(const uint64_t value : stats.lost)
        ok = ok && zcbor_uint64_put(zse, value);

    ok = ok
        && zcbor_list_end_encode(zse, stats.lost.size())
        && zcbor_tstr_put_lit(zse, "filtered")
        && zcbor_uint64_put(zse, stats.filtered);

    return MGMT_RETURN_CHECK(ok);
}

int CanMonitorMgmtGroup::HandleClose(smp_streamer* ctxt) {
    uint32_t token = 0;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    const auto result = service_->Close(token);
    if(!result.has_value())
        return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(result.error()));

    return MGMT_ERR_EOK;
}

int CanMonitorMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::NO_SESSION:
        case Error::BAD_TOKEN:
        case Error::NOT_FOUND:
            return MGMT_ERR_ENOENT;
        case Error::BUSY:
        case Error::RATE:
            return MGMT_ERR_EBUSY;
        case Error::BAD_MODE:
        case Error::BAD_BITRATE:
        case Error::BAD_SEQ:
            return MGMT_ERR_EINVAL;
        case Error::NOT_LISTENING:
        case Error::NOT_ARMED:
        case Error::BUS_OFF:
            return MGMT_ERR_EBADSTATE;
        case Error::NO_MEMORY:
        case Error::NO_ROOM:
            return MGMT_ERR_ENOMEM;
        case Error::RESERVED:
            return MGMT_ERR_EACCESSDENIED;
        case Error::TX_UNSUPPORTED:
            return MGMT_ERR_ENOTSUP;
        case Error::UNKNOWN:
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::can_monitor_domain::smp
