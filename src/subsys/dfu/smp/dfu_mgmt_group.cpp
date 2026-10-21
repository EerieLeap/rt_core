#include <utility>

#include "subsys/dfu/dfu_error.h"

#include "dfu_mgmt_group.h"

namespace eerie_leap::subsys::dfu::smp {

using eerie_leap::subsys::smp::SmpGroupId;

const mgmt_handler DfuMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&DfuMgmtGroup::HandleStatus>, .mh_write = nullptr },
};

DfuMgmtGroup::DfuMgmtGroup(const IDfuStatusSource& status_source) :
    SmpGroup(SmpGroupId::DFU, HANDLERS, TranslateError),
    status_source_(status_source) {}

DfuMgmtGroup::~DfuMgmtGroup() {
    Unregister();
}

int DfuMgmtGroup::HandleStatus(smp_streamer* ctxt) {
    zcbor_state_t* zse = ctxt->writer->zs;

    const DfuStatus status = status_source_.GetStatus();

    bool ok = zcbor_tstr_put_lit(zse, "confirmed")
        && zcbor_bool_put(zse, status.confirmed)
        && zcbor_tstr_put_lit(zse, "state")
        && zcbor_uint32_put(zse, std::to_underlying(status.state));

    if(ok && status.confirm_in_ms.has_value()) {
        ok = zcbor_tstr_put_lit(zse, "confirm_in_ms") && zcbor_uint32_put(zse, *status.confirm_in_ms);
    }

    ok = ok && zcbor_tstr_put_lit(zse, "reset_cause") && zcbor_uint32_put(zse, status.reset_cause);

    return MGMT_RETURN_CHECK(ok);
}

int DfuMgmtGroup::TranslateError(uint16_t error) {
    switch(static_cast<DfuError>(error)) {
        case DfuError::OK:
            return MGMT_ERR_EOK;
        case DfuError::NO_IDENTITY:
        case DfuError::WRONG_PRODUCT:
        case DfuError::WRONG_BOARD:
        case DfuError::BAD_IDENTITY:
            return MGMT_ERR_EINVAL;
        default:
            return MGMT_ERR_EUNKNOWN;
    }
}

} // namespace eerie_leap::subsys::dfu::smp
