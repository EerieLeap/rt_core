#include <array>
#include <chrono>
#include <optional>
#include <utility>

#include "domain/system_domain/models/product_info.h"
#include "domain/system_domain/models/system_configuration.h"

#include "subsys/event_bus/event_channel_id.h"
#ifdef CONFIG_EERIE_LEAP_EVENT_BUS
    #include "subsys/event_bus/event_channel_registry.h"
#endif

#include "subsys/time/boot_id.h"

#include "device_mgmt_group.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::domain::system_domain::models::ProductInfo;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::event_bus::LAST_EVENT_CHANNEL_ID;
using eerie_leap::subsys::smp::SmpDecodeRequest;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::smp::SmpMapKey;
#ifdef CONFIG_EERIE_LEAP_EVENT_BUS
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
#endif

namespace {

// 2020-01-01T00:00:00Z: a client without a clock must not date files 1970.
constexpr uint64_t MIN_WALL_CLOCK_MS = 1577836800000ULL;
// 2100-01-01T00:00:00Z: keeps the conversion to a duration far from overflow.
constexpr uint64_t MAX_WALL_CLOCK_MS = 4102444800000ULL;

} // namespace

const mgmt_handler DeviceMgmtGroup::HANDLERS[] = {
    { .mh_read = Dispatch<&DeviceMgmtGroup::HandleInfo>, .mh_write = nullptr },
    { .mh_read = Dispatch<&DeviceMgmtGroup::HandleTime>, .mh_write = nullptr },
    { .mh_read = nullptr, .mh_write = Dispatch<&DeviceMgmtGroup::HandleSetTime> },
};

DeviceMgmtGroup::DeviceMgmtGroup(
    std::shared_ptr<const ICdmpNetworkInfo> network_info,
    std::optional<SensorCapabilities> sensor_capabilities,
    std::span<const std::string_view> setting_ids,
    std::shared_ptr<ITimeService> time_service
) :
    SmpGroup(SmpGroupId::DEVICE, HANDLERS, nullptr),
    network_info_(std::move(network_info)),
    sensor_capabilities_(sensor_capabilities),
    setting_ids_(setting_ids),
    time_service_(std::move(time_service)) {}

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

int DeviceMgmtGroup::PutTime(smp_streamer* ctxt) const {
    // The three clocks are read back to back, so the client can pair them.
    const auto now = time_service_->GetCurrentTime();
    const auto offset = time_service_->GetWallClockOffset();
    const auto uptime_ms = k_uptime_get();

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    zcbor_state_t* zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "now_ms")
        && zcbor_uint64_put(zse, static_cast<uint64_t>(now_ms))
        && zcbor_tstr_put_lit(zse, "uptime_ms")
        && zcbor_uint64_put(zse, static_cast<uint64_t>(uptime_ms))
        && zcbor_tstr_put_lit(zse, "boot_id")
        && zcbor_uint32_put(zse, eerie_leap::subsys::time::GetBootId());

    if(ok && offset.has_value()) {
        // One truncation, of the sum: truncating both terms could read a millisecond low.
        const auto wall_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>((now + *offset).time_since_epoch()).count();
        ok = zcbor_tstr_put_lit(zse, "wall_ms") && zcbor_uint64_put(zse, static_cast<uint64_t>(wall_ms));
    }

    return MGMT_RETURN_CHECK(ok);
}

int DeviceMgmtGroup::HandleTime(smp_streamer* ctxt) {
    if(time_service_ == nullptr)
        return MGMT_ERR_ENOTSUP;

    return PutTime(ctxt);
}

int DeviceMgmtGroup::HandleSetTime(smp_streamer* ctxt) {
    if(time_service_ == nullptr)
        return MGMT_ERR_ENOTSUP;

    // 0 stands for a missing key; it is below the minimum anyway.
    uint64_t wall_ms = 0;
    std::array keys = { SmpMapKey("wall_ms", zcbor_uint64_decode, &wall_ms) };
    if(!SmpDecodeRequest(ctxt, keys) || wall_ms < MIN_WALL_CLOCK_MS || wall_ms > MAX_WALL_CLOCK_MS)
        return MGMT_ERR_EINVAL;

    time_service_->SetWallClock(
        std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::milliseconds(static_cast<int64_t>(wall_ms))
            )
        )
    );

    return PutTime(ctxt);
}

} // namespace eerie_leap::domain::system_domain::smp
