#include <array>
#include <utility>

#include "network_mgmt_group.h"

namespace eerie_leap::domain::canbus_com_domain::smp {

using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;

namespace {

// A device map, and the ends of the list and the response map, encode to less than this.
constexpr size_t DEVICE_ENTRY_SIZE = 40;

} // namespace

const mgmt_handler NetworkMgmtGroup::HANDLERS[] = {
    {.mh_read = Dispatch<&NetworkMgmtGroup::HandleDevices>, .mh_write = nullptr},
};

NetworkMgmtGroup::NetworkMgmtGroup(std::shared_ptr<const ICdmpNetworkInfo> network_info)
    : SmpGroup(SmpGroupId::NETWORK, HANDLERS, nullptr),
    network_info_(std::move(network_info)) {}

NetworkMgmtGroup::~NetworkMgmtGroup() {
    Unregister();
}

int NetworkMgmtGroup::HandleDevices(smp_streamer* ctxt) {
    uint32_t offset = 0;
    std::array keys = {SmpMapKey("off", zcbor_uint32_decode, &offset)};

    // An empty request lists from the first device.
    const zcbor_state_t* zsd = ctxt->reader->zs;
    if(zsd->payload != zsd->payload_end && !SmpDecodeRequest(ctxt, keys))
        return MGMT_ERR_EINVAL;

    const size_t count = network_info_->GetNetworkDevices(devices_);
    const size_t total = network_info_->GetNetworkDeviceCount();

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "total") && zcbor_uint32_put(zse, static_cast<uint32_t>(total))
        && zcbor_tstr_put_lit(zse, "off") && zcbor_uint32_put(zse, offset)
        && zcbor_tstr_put_lit(zse, "devices") && zcbor_list_start_encode(zse, count);

    for(size_t i = offset; ok && i < count; i++) {
        if(static_cast<size_t>(zse->payload_end - zse->payload) < DEVICE_ENTRY_SIZE)
            break;

        const auto& device = devices_[i];
        ok = zcbor_map_start_encode(zse, 4)
            && zcbor_tstr_put_lit(zse, "id") && zcbor_uint32_put(zse, device.device_id)
            && zcbor_tstr_put_lit(zse, "uid") && zcbor_uint32_put(zse, device.uid)
            && zcbor_tstr_put_lit(zse, "type") && zcbor_uint32_put(zse, std::to_underlying(device.device_type))
            && zcbor_tstr_put_lit(zse, "status") && zcbor_uint32_put(zse, std::to_underlying(device.status))
            && zcbor_map_end_encode(zse, 4);
    }

    ok = ok && zcbor_list_end_encode(zse, count);

    return MGMT_RETURN_CHECK(ok);
}

} // namespace eerie_leap::domain::canbus_com_domain::smp
