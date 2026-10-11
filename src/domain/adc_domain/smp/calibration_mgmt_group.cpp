#include <array>
#include <chrono>
#include <utility>

#include "calibration_mgmt_group.h"

namespace eerie_leap::domain::adc_domain::smp {

using eerie_leap::subsys::smp::SmpAddGroupError;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

const mgmt_handler CalibrationMgmtGroup::HANDLERS[] = {
    {.mh_read = nullptr, .mh_write = Dispatch<&CalibrationMgmtGroup::HandleStart>},
    {.mh_read = nullptr, .mh_write = Dispatch<&CalibrationMgmtGroup::HandleStop>},
    {.mh_read = Dispatch<&CalibrationMgmtGroup::HandleRead>, .mh_write = nullptr},
};

CalibrationMgmtGroup::CalibrationMgmtGroup(std::shared_ptr<CalibrationService> calibration_service, int channel_count)
    : SmpGroup(SmpGroupId::CALIBRATION, HANDLERS, TranslateError),
    calibration_service_(std::move(calibration_service)),
    channel_count_(channel_count) {}

CalibrationMgmtGroup::~CalibrationMgmtGroup() {
    Unregister();
}

int CalibrationMgmtGroup::HandleStart(smp_streamer* ctxt) {
    uint32_t channel = 0;
    std::array keys = {SmpMapKey("channel", zcbor_uint32_decode, &channel)};

    if(!SmpDecodeRequest(ctxt, keys) || !keys[0].found)
        return MGMT_ERR_EINVAL;

    if(channel_count_ <= 0 || channel >= static_cast<uint32_t>(channel_count_))
        return Fail(ctxt, Error::BAD_CHANNEL);

    if(!calibration_service_->Start(static_cast<int>(channel)))
        return Fail(ctxt, Error::BUSY);

    return MGMT_ERR_EOK;
}

int CalibrationMgmtGroup::HandleStop(smp_streamer* ctxt) {
    if(!calibration_service_->Stop())
        return Fail(ctxt, Error::NOT_RUNNING);

    return MGMT_ERR_EOK;
}

int CalibrationMgmtGroup::HandleRead(smp_streamer* ctxt) {
    const bool is_running = calibration_service_->IsRunning();
    const auto channel = calibration_service_->GetChannel();
    const auto latest = calibration_service_->GetLatest();

    // A job stopped before its first sample left nothing to read either.
    if(!channel.has_value() || (!is_running && !latest.has_value()))
        return Fail(ctxt, Error::NOT_RUNNING);

    const CalibrationService::Result result = latest.value_or(CalibrationService::Result{});
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        result.timestamp.time_since_epoch()).count();

    zcbor_state_t* zse = ctxt->writer->zs;
    const bool ok = zcbor_tstr_put_lit(zse, "channel") && zcbor_uint32_put(zse, static_cast<uint32_t>(*channel))
        && zcbor_tstr_put_lit(zse, "adc_v") && zcbor_float32_put(zse, result.adc_volts)
        && zcbor_tstr_put_lit(zse, "sensor_v") && zcbor_float32_put(zse, result.sensor_volts)
        && zcbor_tstr_put_lit(zse, "ts") && zcbor_uint64_put(zse, static_cast<uint64_t>(timestamp_ms))
        && zcbor_tstr_put_lit(zse, "seq") && zcbor_uint32_put(zse, result.sequence)
        && zcbor_tstr_put_lit(zse, "adc_err") && zcbor_int32_put(zse, std::to_underlying(result.error))
        && zcbor_tstr_put_lit(zse, "running") && zcbor_bool_put(zse, is_running);

    return MGMT_RETURN_CHECK(ok);
}

int CalibrationMgmtGroup::Fail(smp_streamer* ctxt, Error error) {
    return SmpAddGroupError(ctxt, SmpGroupId::CALIBRATION, std::to_underlying(error));
}

int CalibrationMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<Error>(error)) {
        case Error::OK:
            return MGMT_ERR_EOK;
        case Error::BAD_CHANNEL:
            return MGMT_ERR_EINVAL;
        case Error::BUSY:
            return MGMT_ERR_EBUSY;
        case Error::NOT_RUNNING:
            return MGMT_ERR_EBADSTATE;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::domain::adc_domain::smp
