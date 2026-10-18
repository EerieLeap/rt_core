#include <optional>
#include <utility>

#include "domain/system_domain/models/product_info.h"
#include "domain/system_domain/models/system_configuration.h"

#include "device_mgmt_group.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::domain::system_domain::models::ProductInfo;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::smp::SmpGroupId;

const mgmt_handler DeviceMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&DeviceMgmtGroup::HandleInfo>, .mh_write = nullptr },
};

DeviceMgmtGroup::DeviceMgmtGroup(
    uint32_t build_number,
    std::shared_ptr<const ICdmpNetworkInfo> network_info,
    std::optional<SensorCapabilities> sensor_capabilities
) :
    SmpGroup(SmpGroupId::DEVICE, HANDLERS, nullptr),
    build_number_(build_number),
    network_info_(std::move(network_info)),
    sensor_capabilities_(sensor_capabilities) {}

DeviceMgmtGroup::~DeviceMgmtGroup() {
    Unregister();
}

int DeviceMgmtGroup::HandleInfo(smp_streamer* ctxt) {
    zcbor_state_t* zse = ctxt->writer->zs;

    std::optional<CdmpDeviceInfo> device;
    if(network_info_ != nullptr)
        device = network_info_->GetDeviceInfo();

    const uint16_t features = device.has_value() ? device->features : ProductInfo::FEATURES;

    bool ok = zcbor_tstr_put_lit(zse, "product")
        && zcbor_uint32_put(zse, ProductInfo::PRODUCT_ID)
        && zcbor_tstr_put_lit(zse, "revision")
        && zcbor_uint32_put(zse, ProductInfo::REVISION)
        && zcbor_tstr_put_lit(zse, "features")
        && zcbor_uint32_put(zse, features)
        && zcbor_tstr_put_lit(zse, "hw")
        && zcbor_uint32_put(zse, SystemConfiguration::HW_VERSION)
        && zcbor_tstr_put_lit(zse, "sw")
        && zcbor_uint32_put(zse, SystemConfiguration::SW_VERSION)
        && zcbor_tstr_put_lit(zse, "build")
        && zcbor_uint32_put(zse, build_number_);

    if(ok && device.has_value()) {
        ok = zcbor_tstr_put_lit(zse, "cdmp_id")
            && zcbor_uint32_put(zse, device->device_id)
            && zcbor_tstr_put_lit(zse, "uid")
            && zcbor_uint32_put(zse, device->uid)
            && zcbor_tstr_put_lit(zse, "status")
            && zcbor_uint32_put(zse, std::to_underlying(device->status));
    }

    if(ok && sensor_capabilities_.has_value()) {
        ok = zcbor_tstr_put_lit(zse, "adc_channels")
            && zcbor_uint32_put(zse, sensor_capabilities_->adc_channels)
            && zcbor_tstr_put_lit(zse, "gpio_channels")
            && zcbor_uint32_put(zse, sensor_capabilities_->gpio_channels)
            && zcbor_tstr_put_lit(zse, "sensors_max")
            && zcbor_uint32_put(zse, sensor_capabilities_->sensors_max);
    }

    return MGMT_RETURN_CHECK(ok);
}

} // namespace eerie_leap::domain::system_domain::smp
