#include <array>
#include <optional>
#include <utility>

#include "domain/system_domain/models/product_info.h"
#include "domain/system_domain/models/system_configuration.h"

#include "subsys/event_bus/event_channel_id.h"
#ifdef CONFIG_EERIE_LEAP_EVENT_BUS
    #include "subsys/event_bus/event_channel_registry.h"
#endif

#include "device_mgmt_group.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::domain::system_domain::models::ProductInfo;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::event_bus::LAST_EVENT_CHANNEL_ID;
using eerie_leap::subsys::smp::SmpGroupId;
#ifdef CONFIG_EERIE_LEAP_EVENT_BUS
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
#endif

const mgmt_handler DeviceMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&DeviceMgmtGroup::HandleInfo>, .mh_write = nullptr },
};

DeviceMgmtGroup::DeviceMgmtGroup(
    std::shared_ptr<const ICdmpNetworkInfo> network_info,
    std::optional<SensorCapabilities> sensor_capabilities,
    std::span<const std::string_view> setting_ids
) :
    SmpGroup(SmpGroupId::DEVICE, HANDLERS, nullptr),
    network_info_(std::move(network_info)),
    sensor_capabilities_(sensor_capabilities),
    setting_ids_(setting_ids) {}

DeviceMgmtGroup::~DeviceMgmtGroup() {
    Unregister();
}

bool DeviceMgmtGroup::PutEventChannels(zcbor_state_t* zse) {
    // No allocation in a handler: a fixed table sized from the last id.
    std::array<uint32_t, std::to_underlying(LAST_EVENT_CHANNEL_ID)> ids{};
    size_t count = 0; // NOLINT(misc-const-correctness): incremented under CONFIG_EERIE_LEAP_EVENT_BUS

#ifdef CONFIG_EERIE_LEAP_EVENT_BUS
    const auto& registry = EventChannelRegistry::GetInstance();
    for(uint32_t id = 1; id <= std::to_underlying(LAST_EVENT_CHANNEL_ID); id++) {
        if(registry.Find(static_cast<EventChannelId>(id)) != nullptr)
            ids.at(count++) = id;
    }
#endif

    if(!zcbor_tstr_put_lit(zse, "event_channels") || !zcbor_list_start_encode(zse, count))
        return false;

    for(size_t i = 0; i < count; i++) {
        if(!zcbor_uint32_put(zse, ids.at(i)))
            return false;
    }

    return zcbor_list_end_encode(zse, count);
}

bool DeviceMgmtGroup::PutSettings(zcbor_state_t* zse) const {
    if(!zcbor_tstr_put_lit(zse, "settings") || !zcbor_list_start_encode(zse, setting_ids_.size()))
        return false;

    for(const std::string_view id : setting_ids_) {
        if(!zcbor_tstr_encode_ptr(zse, id.data(), id.size()))
            return false;
    }

    return zcbor_list_end_encode(zse, setting_ids_.size());
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
        && zcbor_uint32_put(zse, SystemConfiguration::SW_BUILD)
        && zcbor_tstr_put_lit(zse, "board")
        && zcbor_tstr_put_lit(zse, CONFIG_BOARD_TARGET);

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

    if(ok)
        ok = PutEventChannels(zse);

    if(ok)
        ok = PutSettings(zse);

    return MGMT_RETURN_CHECK(ok);
}

} // namespace eerie_leap::domain::system_domain::smp
