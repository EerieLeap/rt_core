#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/smp/network_mgmt_group.h"
#include "domain/system_domain/models/system_configuration.h"
#include "domain/system_domain/smp/device_mgmt_group.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/boot_id.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "cdmp_test_features.h"
#include "smp_groups_test_support.h"

using eerie_leap::tests::DISPLAY_FEATURES;
using eerie_leap::tests::LOGGER_FEATURES;

using namespace smp_groups_test;

using eerie_leap::domain::canbus_com_domain::smp::NetworkMgmtGroup;
using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::domain::system_domain::smp::DeviceMgmtGroup;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;

namespace {

CdmpDeviceInfo MakeDevice(uint8_t device_id) {
    return {
        .device_id = device_id,
        .uid = 0x10000000U + device_id,
        .features = LOGGER_FEATURES,
        .status = CdmpDeviceStatus::ONLINE,
    };
}

} // namespace

ZTEST_SUITE(smp_device_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_device_group, test_info_reports_product_versions_and_cdmp_identity) {
    const auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->self = {
        .device_id = 7,
        .uid = 0xCAFEBABE,
        .features = DISPLAY_FEATURES,
        .status = CdmpDeviceStatus::ONLINE,
    };
    DeviceMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_equal(response.Value("product"), 4660);
    zassert_equal(response.Value("revision"), 3);
    zassert_equal(response.Value("features"), DISPLAY_FEATURES, "The mask the unit sends on CDMP");
    zassert_false(response.Has("family"));
    zassert_equal(response.Value("hw"), (1U << 24) | (2U << 16) | 3U);
    zassert_equal(response.Value("sw"), (4U << 24) | (5U << 16) | 6U);
    zassert_equal(response.Value("build"), SystemConfiguration::SW_BUILD);
    zassert_equal(response.Value("build"), 7, "The VERSION tweak, set by prj.conf here");
    zassert_equal(response.texts.at("board"), CONFIG_BOARD_TARGET);
    zassert_equal(response.Value("cdmp_id"), 7);
    zassert_equal(response.Value("uid"), 0xCAFEBABE);
    zassert_false(response.Has("device_type"));
    zassert_equal(response.Value("status"), std::to_underlying(CdmpDeviceStatus::ONLINE));
}

ZTEST(smp_device_group, test_info_without_cdmp_has_no_identity) {
    DeviceMgmtGroup group(nullptr);
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_true(response.Has("product"));
    zassert_equal(response.Value("features"), 1 << 2, "The build's features without CDMP");
    zassert_false(response.Has("cdmp_id"));
    zassert_false(response.Has("adc_channels"), "No sensor inputs on a unit without sensors");
}

ZTEST(smp_device_group, test_info_reports_the_sensor_inputs) {
    DeviceMgmtGroup group(nullptr, SensorCapabilities{ .adc_channels = 4, .gpio_channels = 8, .sensors_max = 64 });
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_equal(response.Value("adc_channels"), 4);
    zassert_equal(response.Value("gpio_channels"), 8);
    zassert_equal(response.Value("sensors_max"), 64);
}

namespace {

enum class DeviceTestEvent : uint32_t {
    Updated = 0
};
enum class DeviceTestPayload : uint32_t {
    Value = 0
};

class DeviceTestChannel : public eerie_leap::subsys::event_bus::EventChannel<DeviceTestEvent, DeviceTestPayload> {
public:
    explicit DeviceTestChannel(std::string name) :
        EventChannel(std::move(name), DeviceTestEvent::Updated, DeviceTestPayload::Value) {}
};

} // namespace

// The registry is one per binary, so this registers the channels it expects and no other test lists them.
ZTEST(smp_device_group, test_info_lists_the_registered_event_channels_and_settings) {
    using eerie_leap::subsys::event_bus::EventChannelId;
    using eerie_leap::subsys::event_bus::EventChannelRegistry;

    static DeviceTestChannel sensors("device_test_sensors");
    static DeviceTestChannel logging("device_test_logging");
    auto& registry = EventChannelRegistry::GetInstance();
    zassert_ok(registry.Register(EventChannelId::Sensors, sensors));
    zassert_ok(registry.Register(EventChannelId::Logging, logging));

    static constexpr std::array<std::string_view, 2> SETTINGS{ "display.brightness", "display.contrast" };
    DeviceMgmtGroup group(nullptr, std::nullopt, SETTINGS);
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    const std::vector<uint32_t> channels{ 1, 2 };
    zassert_equal(response.uint_lists.at("event_channels"), channels, "Navigation and Settings are not registered");
    const std::vector<std::string> settings{ "display.brightness", "display.contrast" };
    zassert_equal(response.text_lists.at("settings"), settings);
}

ZTEST(smp_device_group, test_info_lists_no_settings_by_default) {
    DeviceMgmtGroup group(nullptr);
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_equal(response.list_sizes.at("settings"), 0, "The key is there, empty");
    zassert_true(response.list_sizes.contains("event_channels"), "Always present, whatever is registered");
}

namespace {

constexpr uint64_t WALL_2027_MS = 1800000000000ULL;
constexpr uint64_t WALL_2020_MS = 1577836800000ULL;

std::shared_ptr<TimeService> MakeTimeService() {
    return std::make_shared<TimeService>(std::make_shared<RtcProvider>(), std::make_shared<BootElapsedTimeProvider>());
}

} // namespace

ZTEST(smp_device_group, test_time_reports_the_clocks_without_wall_time_until_it_is_set) {
    DeviceMgmtGroup group(nullptr, std::nullopt, {}, MakeTimeService());
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::TIME));

    zassert_true(response.IsOk());
    zassert_true(response.WideValue("now_ms") >= 1761106217000ULL, "The unit's clock: a fixed date plus the uptime");
    zassert_true(response.Has("uptime_ms"));
    zassert_equal(response.Value("boot_id"), eerie_leap::subsys::time::GetBootId());
    zassert_not_equal(response.Value("boot_id"), 0);
    zassert_false(response.Has("wall_ms"), "Unset at power-on");
}

ZTEST(smp_device_group, test_set_time_answers_as_time_with_the_wall_time_just_set) {
    DeviceMgmtGroup group(nullptr, std::nullopt, {}, MakeTimeService());
    group.Register();
    SmpTestClient client;

    const auto set = client.Write(
        SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::SET_TIME),
        CborMap().PutWide("wall_ms", WALL_2027_MS).Build()
    );

    zassert_true(set.IsOk());
    zassert_true(set.Has("now_ms"));
    zassert_true(set.Has("uptime_ms"));
    zassert_equal(set.Value("boot_id"), eerie_leap::subsys::time::GetBootId());
    const uint64_t answered = set.WideValue("wall_ms");
    zassert_true(answered >= WALL_2027_MS && answered - WALL_2027_MS < 50, "The answer follows the set");

    const auto read = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::TIME));

    zassert_true(read.IsOk());
    const uint64_t wall_ms = read.WideValue("wall_ms");
    zassert_true(wall_ms >= answered && wall_ms - answered < 50, "Real time keeps running with the clock");
    zassert_equal(wall_ms - read.WideValue("now_ms"), answered - set.WideValue("now_ms"), "A fixed offset");
}

ZTEST(smp_device_group, test_set_time_does_not_step_the_unit_clock) {
    DeviceMgmtGroup group(nullptr, std::nullopt, {}, MakeTimeService());
    group.Register();
    SmpTestClient client;
    const auto before = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::TIME));

    zassert_true(client
            .Write(
                SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::SET_TIME),
                CborMap().PutWide("wall_ms", WALL_2027_MS).Build()
            )
            .IsOk());
    const auto after = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::TIME));

    const uint64_t clock_gap = after.WideValue("now_ms") - before.WideValue("now_ms");
    zassert_true(clock_gap < 1000, "now_ms moved only by the time that passed");
    const int64_t boot_before = static_cast<int64_t>(before.WideValue("now_ms") - before.WideValue("uptime_ms"));
    const int64_t boot_after = static_cast<int64_t>(after.WideValue("now_ms") - after.WideValue("uptime_ms"));
    zassert_true(std::abs(boot_after - boot_before) <= 5, "now_ms minus uptime_ms stays the clock at power-on");
}

ZTEST(smp_device_group, test_a_second_set_time_replaces_the_first) {
    DeviceMgmtGroup group(nullptr, std::nullopt, {}, MakeTimeService());
    group.Register();
    SmpTestClient client;

    zassert_true(client
            .Write(
                SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::SET_TIME),
                CborMap().PutWide("wall_ms", WALL_2027_MS).Build()
            )
            .IsOk());
    const auto second = client.Write(
        SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::SET_TIME),
        CborMap().PutWide("wall_ms", WALL_2027_MS + 3600000ULL).Build()
    );

    zassert_true(second.IsOk());
    const uint64_t wall_ms = second.WideValue("wall_ms");
    zassert_true(wall_ms >= WALL_2027_MS + 3600000ULL && wall_ms - (WALL_2027_MS + 3600000ULL) < 50);
}

ZTEST(smp_device_group, test_set_time_refuses_a_missing_or_early_wall_time) {
    const auto time_service = MakeTimeService();
    DeviceMgmtGroup group(nullptr, std::nullopt, {}, time_service);
    group.Register();
    SmpTestClient client;
    const auto command = std::to_underlying(DeviceMgmtGroup::Command::SET_TIME);

    const auto missing = client.Write(SmpGroupId::DEVICE, command, CborMap().Build());
    zassert_equal(missing.Value("rc"), MGMT_ERR_EINVAL);

    const auto zero = client.Write(SmpGroupId::DEVICE, command, CborMap().PutWide("wall_ms", 0).Build());
    zassert_equal(zero.Value("rc"), MGMT_ERR_EINVAL);

    const auto early = client.Write(
        SmpGroupId::DEVICE, command, CborMap().PutWide("wall_ms", WALL_2020_MS - 1).Build()
    );
    zassert_equal(early.Value("rc"), MGMT_ERR_EINVAL);
    zassert_false(time_service->GetWallClockOffset().has_value(), "A refused set changes nothing");

    const auto late = client.Write(SmpGroupId::DEVICE, command, CborMap().PutWide("wall_ms", 4102444800001ULL).Build());
    zassert_equal(late.Value("rc"), MGMT_ERR_EINVAL, "Above 2100-01-01T00:00:00Z");

    const auto last = client.Write(SmpGroupId::DEVICE, command, CborMap().PutWide("wall_ms", 4102444800000ULL).Build());
    zassert_true(last.IsOk(), "2100-01-01T00:00:00Z itself is accepted");

    const auto boundary = client.Write(SmpGroupId::DEVICE, command, CborMap().PutWide("wall_ms", WALL_2020_MS).Build());
    zassert_true(boundary.IsOk(), "2020-01-01T00:00:00Z itself is accepted");
}

ZTEST(smp_device_group, test_time_commands_are_not_supported_without_a_time_service) {
    DeviceMgmtGroup group(nullptr);
    group.Register();
    SmpTestClient client;

    const auto time = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::TIME));
    zassert_equal(time.Value("rc"), MGMT_ERR_ENOTSUP);

    const auto set = client.Write(
        SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::SET_TIME),
        CborMap().PutWide("wall_ms", WALL_2027_MS).Build()
    );
    zassert_equal(set.Value("rc"), MGMT_ERR_ENOTSUP);
}

ZTEST(smp_device_group, test_only_one_instance_is_registered) {
    DeviceMgmtGroup first(nullptr);
    DeviceMgmtGroup second(nullptr);
    first.Register();

    bool threw = false;
    try {
        second.Register();
    } catch(const std::logic_error&) {
        threw = true;
    }

    zassert_true(threw);
    zassert_true(first.IsRegistered());

    first.Unregister();
    second.Register();
    zassert_true(second.IsRegistered(), "The group can move to another instance once unregistered");
}

ZTEST_SUITE(smp_network_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_network_group, test_devices_lists_the_network) {
    const auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->devices = { MakeDevice(2), MakeDevice(5), MakeDevice(9) };
    NetworkMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    auto response = client.Read(SmpGroupId::NETWORK, std::to_underlying(NetworkMgmtGroup::Command::DEVICES));

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 3);
    zassert_equal(response.Value("off"), 0);
    zassert_equal(response.entries.size(), 3);
    zassert_equal(response.entries[0]["id"], 2);
    zassert_equal(response.entries[0]["uid"], 0x10000002);
    zassert_equal(response.entries[0]["features"], LOGGER_FEATURES);
    zassert_equal(response.entries[0]["status"], std::to_underlying(CdmpDeviceStatus::ONLINE));
    zassert_equal(response.entries[2]["id"], 9);
}

ZTEST(smp_network_group, test_empty_request_lists_from_the_first_device) {
    const auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->devices = { MakeDevice(4) };
    NetworkMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    const auto response = client.Read(SmpGroupId::NETWORK, std::to_underlying(NetworkMgmtGroup::Command::DEVICES), {});

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 1);
}

ZTEST(smp_network_group, test_a_large_network_is_paged) {
    const auto network_info = std::make_shared<FakeNetworkInfo>();
    for(uint8_t id = 1; id <= 50; id++)
        network_info->devices.push_back(MakeDevice(id));
    NetworkMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    std::vector<uint32_t> ids;
    size_t pages = 0;
    while(ids.size() < 50) {
        auto response = client.Read(
            SmpGroupId::NETWORK, std::to_underlying(NetworkMgmtGroup::Command::DEVICES),
            CborMap().Put("off", ids.size()).Build()
        );
        zassert_true(response.IsOk());
        zassert_equal(response.Value("total"), 50);
        zassert_false(response.entries.empty());

        for(auto& entry : response.entries)
            ids.push_back(entry["id"]);
        pages++;
    }

    zassert_true(pages > 1, "50 devices do not fit into one response");
    for(size_t i = 0; i < ids.size(); i++)
        zassert_equal(ids[i], i + 1);
}
