#include <array>
#include <span>
#include <utility>

#include "live_mgmt_group.h"

namespace eerie_leap::domain::ble_domain::smp {

using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

using SubscribeResult = LiveDataService::SubscribeResult;

namespace {

bool DecodeSensorIds(zcbor_state_t* zsd, LiveMgmtGroup::SensorIdList* list) {
    list->count = 0;

    if(!zcbor_list_start_decode(zsd))
        return false;

    while(!zcbor_array_at_end(zsd)) {
        uint32_t hash = 0;
        if(!zcbor_uint32_decode(zsd, &hash))
            return false;

        if(list->count < list->hashes.size())
            list->hashes[list->count] = hash;
        list->count++;
    }

    return zcbor_list_end_decode(zsd);
}

} // namespace

const mgmt_handler LiveMgmtGroup::HANDLERS[] = {
    {.mh_read = nullptr, .mh_write = Dispatch<&LiveMgmtGroup::HandleSubscribe>},
    {.mh_read = nullptr, .mh_write = Dispatch<&LiveMgmtGroup::HandleUnsubscribe>},
};

LiveMgmtGroup::LiveMgmtGroup(std::shared_ptr<LiveDataService> live_data_service)
    : SmpGroup(SmpGroupId::LIVE, HANDLERS, TranslateError),
    live_data_service_(std::move(live_data_service)) {}

LiveMgmtGroup::~LiveMgmtGroup() {
    Unregister();
}

int LiveMgmtGroup::HandleSubscribe(smp_streamer* ctxt) {
    uint32_t period_ms = 0;
    std::array keys = {
        SmpMapKey("ids", DecodeSensorIds, &sensor_ids_),
        SmpMapKey("period", zcbor_uint32_decode, &period_ms),
    };

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found || !keys[1].found || sensor_ids_.count == 0)
        return MGMT_ERR_EINVAL;

    const size_t capacity = live_data_service_->GetCapacity();

    SubscribeResult result = SubscribeResult::NOT_LISTENING;
    if(capacity > 0) {
        result = sensor_ids_.count <= sensor_ids_.hashes.size()
            ? live_data_service_->Subscribe(std::span(sensor_ids_.hashes).first(sensor_ids_.count), period_ms)
            : SubscribeResult::TOO_MANY;
    }

    zcbor_state_t* zse = ctxt->writer->zs;

    switch(result) {
        case SubscribeResult::OK: {
            const bool ok = zcbor_tstr_put_lit(zse, "period")
                && zcbor_uint32_put(zse, LiveDataService::ClampPeriod(period_ms))
                && zcbor_tstr_put_lit(zse, "max") && zcbor_uint32_put(zse, static_cast<uint32_t>(capacity));

            return MGMT_RETURN_CHECK(ok);
        }

        // The client learns how many sensors to ask for.
        case SubscribeResult::TOO_MANY: {
            const bool ok = smp_add_cmd_err(zse, std::to_underlying(SmpGroupId::LIVE), std::to_underlying(Error::TOO_MANY))
                && zcbor_tstr_put_lit(zse, "max") && zcbor_uint32_put(zse, static_cast<uint32_t>(capacity));

            return MGMT_RETURN_CHECK(ok);
        }

        case SubscribeResult::NOT_LISTENING:
            return SmpAddGroupError(ctxt, SmpGroupId::LIVE, std::to_underlying(Error::NOT_LISTENING));
    }

    return MGMT_ERR_EUNKNOWN;
}

int LiveMgmtGroup::HandleUnsubscribe(smp_streamer* /*ctxt*/) {
    live_data_service_->Unsubscribe();

    return MGMT_ERR_EOK;
}

int LiveMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::TOO_MANY:
            return MGMT_ERR_EINVAL;
        case Error::NOT_LISTENING:
            return MGMT_ERR_EBADSTATE;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::ble_domain::smp
