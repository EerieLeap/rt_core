#include <utility>

#include "domain/system_domain/models/product_info.h"
#include "domain/system_domain/models/system_configuration.h"

#include "device_mgmt_group.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::domain::system_domain::models::ProductInfo;
using eerie_leap::domain::system_domain::models::SystemConfiguration;

const mgmt_handler DeviceMgmtGroup::HANDLERS[] = {
    {.mh_read = Dispatch<&DeviceMgmtGroup::HandleInfo>, .mh_write = nullptr},
};

DeviceMgmtGroup::DeviceMgmtGroup(uint32_t build_number, std::shared_ptr<const ICdmpNetworkInfo> network_info)
    : SmpGroup(SmpGroupId::DEVICE, HANDLERS, nullptr),
    build_number_(build_number),
    network_info_(std::move(network_info)) {}

DeviceMgmtGroup::~DeviceMgmtGroup() {
    Unregister();
}

int DeviceMgmtGroup::HandleInfo(smp_streamer* ctxt) {
    zcbor_state_t* zse = ctxt->writer->zs;

    bool ok = zcbor_tstr_put_lit(zse, "family") && zcbor_uint32_put(zse, std::to_underlying(ProductInfo::family))
        && zcbor_tstr_put_lit(zse, "product") && zcbor_uint32_put(zse, ProductInfo::product_id)
        && zcbor_tstr_put_lit(zse, "revision") && zcbor_uint32_put(zse, ProductInfo::revision)
        && zcbor_tstr_put_lit(zse, "features") && zcbor_uint32_put(zse, ProductInfo::features)
        && zcbor_tstr_put_lit(zse, "hw") && zcbor_uint32_put(zse, SystemConfiguration::hw_version)
        && zcbor_tstr_put_lit(zse, "sw") && zcbor_uint32_put(zse, SystemConfiguration::sw_version)
        && zcbor_tstr_put_lit(zse, "build") && zcbor_uint32_put(zse, build_number_);

    if(ok && network_info_ != nullptr) {
        const auto device = network_info_->GetDeviceInfo();

        ok = zcbor_tstr_put_lit(zse, "cdmp_id") && zcbor_uint32_put(zse, device.device_id)
            && zcbor_tstr_put_lit(zse, "uid") && zcbor_uint32_put(zse, device.uid)
            && zcbor_tstr_put_lit(zse, "device_type") && zcbor_uint32_put(zse, std::to_underlying(device.device_type))
            && zcbor_tstr_put_lit(zse, "status") && zcbor_uint32_put(zse, std::to_underlying(device.status));
    }

    return MGMT_RETURN_CHECK(ok);
}

} // namespace eerie_leap::domain::system_domain::smp
