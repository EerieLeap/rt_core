#include "can_monitor_mgmt_group.h"

#include <algorithm>
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

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX

// A frame as SEND and PERIODIC write it: the data is a byte string of at most 64 bytes.
bool StoreTxData(const zcbor_string& data, models::CanMonitorTxFrame& frame, bool& is_valid) {
    if(data.len > frame.data.size()) {
        is_valid = false;
        return true;
    }

    frame.size = static_cast<uint8_t>(data.len);
    std::copy_n(data.value, data.len, frame.data.begin());

    return true;
}

// One frame: [id, flags, data].
bool DecodeSendFrame(zcbor_state_t* zsd, models::CanMonitorTxFrame& frame, bool& is_valid) {
    uint32_t flags = 0;
    zcbor_string data{};

    if(!zcbor_list_start_decode(zsd)
        || !zcbor_uint32_decode(zsd, &frame.id)
        || !zcbor_uint32_decode(zsd, &flags)
        || !zcbor_bstr_decode(zsd, &data)
        || !zcbor_list_end_decode(zsd))
        return false;

    if(flags > UINT8_MAX)
        is_valid = false;
    frame.flags = static_cast<uint8_t>(flags);

    return StoreTxData(data, frame, is_valid);
}

bool DecodeSendFrames(zcbor_state_t* zsd, CanMonitorMgmtGroup::TxFrameList* list) {
    list->count = 0;
    list->is_valid = true;

    if(!zcbor_list_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        models::CanMonitorTxFrame frame;
        if(!DecodeSendFrame(zsd, frame, list->is_valid))
            return false;

        if(list->count < list->frames.size())
            list->frames[list->count] = frame;
        list->count++;
    }

    return zcbor_list_end_decode(zsd);
}

// One entry: {slot, id, flags, data, period_ms, ? count}.
bool DecodePeriodicEntry(zcbor_state_t* zsd, models::CanMonitorPeriodic& entry, bool& is_valid) {
    uint32_t flags = 0;
    uint32_t count = 0;
    zcbor_string data{};

    std::array keys = {
        SmpMapKey("slot", zcbor_uint32_decode, &entry.slot),
        SmpMapKey("id", zcbor_uint32_decode, &entry.frame.id),
        SmpMapKey("flags", zcbor_uint32_decode, &flags),
        SmpMapKey("data", zcbor_bstr_decode, &data),
        SmpMapKey("period_ms", zcbor_uint32_decode, &entry.period_ms),
        SmpMapKey("count", zcbor_uint32_decode, &count),
    };

    size_t decoded = 0;
    if(zcbor_map_decode_bulk(zsd, keys.data(), keys.size(), &decoded) != 0)
        return false;

    // Everything but `count` is required.
    if(!(keys[0].found && keys[1].found && keys[2].found && keys[3].found && keys[4].found) || flags > UINT8_MAX)
        is_valid = false;

    entry.frame.flags = static_cast<uint8_t>(flags);
    if(keys[5].found)
        entry.count = count;

    return StoreTxData(data, entry.frame, is_valid);
}

bool DecodePeriodicList(zcbor_state_t* zsd, CanMonitorMgmtGroup::PeriodicList* list) {
    list->count = 0;
    list->is_valid = true;

    if(!zcbor_list_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        models::CanMonitorPeriodic entry;
        if(!DecodePeriodicEntry(zsd, entry, list->is_valid))
            return false;

        if(list->count < list->entries.size())
            list->entries[list->count] = entry;
        list->count++;
    }

    return zcbor_list_end_decode(zsd);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

// A request the service could not act on is malformed; every other error is the group's own.
[[maybe_unused]] int AnswerError(const smp_streamer* ctxt, models::CanMonitorError error) {
    if(error == models::CanMonitorError::INVALID)
        return MGMT_ERR_EINVAL;

    return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(error));
}

} // namespace

const mgmt_handler CanMonitorMgmtGroup::HANDLERS[COMMAND_COUNT] = {
    { .mh_read = Dispatch<&CanMonitorMgmtGroup::HandleStatus>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleOpen> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleClose> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleFilter> },
    { .mh_read = Dispatch<&CanMonitorMgmtGroup::HandleStats>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleArm> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleSend> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandlePeriodic> },
    { .mh_read = nullptr, .mh_write = Dispatch<&CanMonitorMgmtGroup::HandleReplay> },
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

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX

int CanMonitorMgmtGroup::HandleArm(smp_streamer* ctxt) {
    uint32_t token = 0;
    bool on = false;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("on", zcbor_bool_decode, &on),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    const auto result = service_->Arm(token, on);
    if(!result.has_value())
        return AnswerError(ctxt, result.error());

    zcbor_state_t* zse = ctxt->writer->zs;

    bool ok = zcbor_tstr_put_lit(zse, "armed")
        && zcbor_bool_put(zse, result->is_armed)
        && zcbor_tstr_put_lit(zse, "share_pct")
        && zcbor_uint32_put(zse, result->share_pct)
        && zcbor_tstr_put_lit(zse, "reserved")
        && zcbor_list_start_encode(zse, 2);

    for(const models::CanMonitorReservedRange& range : result->reserved) {
        ok = ok
            && zcbor_list_start_encode(zse, 3)
            && zcbor_uint32_put(zse, range.from)
            && zcbor_uint32_put(zse, range.to)
            && zcbor_bool_put(zse, range.ext)
            && zcbor_list_end_encode(zse, 3);
    }

    return MGMT_RETURN_CHECK(ok && zcbor_list_end_encode(zse, 2));
}

int CanMonitorMgmtGroup::HandleSend(smp_streamer* ctxt) {
    uint32_t token = 0;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("frames", DecodeSendFrames, &tx_frame_list_),
    };
    tx_frame_list_.count = 0;
    tx_frame_list_.is_valid = true;

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    if(!tx_frame_list_.is_valid || tx_frame_list_.count == 0 || tx_frame_list_.count > tx_frame_list_.frames.size())
        return MGMT_ERR_EINVAL;

    const auto result = service_->Send(
        token, std::span<const models::CanMonitorTxFrame>(tx_frame_list_.frames).first(tx_frame_list_.count)
    );
    if(!result.has_value())
        return AnswerError(ctxt, result.error());

    zcbor_state_t* zse = ctxt->writer->zs;

    return MGMT_RETURN_CHECK(
        zcbor_tstr_put_lit(zse, "sent")
        && zcbor_uint32_put(zse, result->sent)
        && zcbor_tstr_put_lit(zse, "refused")
        && zcbor_uint32_put(zse, result->refused)
    );
}

int CanMonitorMgmtGroup::HandlePeriodic(smp_streamer* ctxt) {
    uint32_t token = 0;
    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("list", DecodePeriodicList, &periodic_list_),
    };
    periodic_list_.count = 0;
    periodic_list_.is_valid = true;

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found)
        return MGMT_ERR_EINVAL;

    if(!periodic_list_.is_valid || periodic_list_.count > periodic_list_.entries.size())
        return MGMT_ERR_EINVAL;

    const auto result = service_->SetPeriodic(
        token, std::span<const models::CanMonitorPeriodic>(periodic_list_.entries).first(periodic_list_.count)
    );
    if(!result.has_value())
        return AnswerError(ctxt, result.error());

    return MGMT_ERR_EOK;
}

int CanMonitorMgmtGroup::HandleReplay(smp_streamer* ctxt) {
    uint32_t token = 0;
    uint32_t op = 0;
    uint32_t speed_pct = 100;
    bool is_loop = false;
    uint32_t seq = 0;
    zcbor_string data{};

    std::array keys = {
        SmpMapKey("tok", zcbor_uint32_decode, &token),
        SmpMapKey("op", zcbor_uint32_decode, &op),
        SmpMapKey("speed_pct", zcbor_uint32_decode, &speed_pct),
        SmpMapKey("loop", zcbor_bool_decode, &is_loop),
        SmpMapKey("seq", zcbor_uint32_decode, &seq),
        SmpMapKey("data", zcbor_bstr_decode, &data),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || op >= models::CAN_MONITOR_REPLAY_OP_COUNT)
        return MGMT_ERR_EINVAL;

    models::CanMonitorReplayRequest request;
    request.op = static_cast<models::CanMonitorReplayOp>(op);
    request.speed_pct = speed_pct;
    request.is_loop = is_loop;

    if(request.op == models::CanMonitorReplayOp::CHUNK) {
        if(!keys[4].found || !keys[5].found || seq > UINT16_MAX)
            return MGMT_ERR_EINVAL;

        request.seq = static_cast<uint16_t>(seq);
        request.data = std::span<const uint8_t>(data.value, data.len);
    }

    const auto result = service_->Replay(token, request);
    if(!result.has_value())
        return AnswerError(ctxt, result.error());

    zcbor_state_t* zse = ctxt->writer->zs;

    return MGMT_RETURN_CHECK(zcbor_tstr_put_lit(zse, "room") && zcbor_uint32_put(zse, *result));
}

#else // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

int CanMonitorMgmtGroup::HandleArm(smp_streamer* ctxt) {
    return SmpAddGroupError(ctxt, SmpGroupId::CAN_MONITOR, std::to_underlying(Error::TX_UNSUPPORTED));
}

int CanMonitorMgmtGroup::HandleSend(smp_streamer* ctxt) {
    return HandleArm(ctxt);
}

int CanMonitorMgmtGroup::HandlePeriodic(smp_streamer* ctxt) {
    return HandleArm(ctxt);
}

int CanMonitorMgmtGroup::HandleReplay(smp_streamer* ctxt) {
    return HandleArm(ctxt);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

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
