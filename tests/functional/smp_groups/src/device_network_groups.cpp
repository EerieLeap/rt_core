#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#include "domain/system_domain/smp/device_mgmt_group.h"
#include "domain/canbus_com_domain/smp/network_mgmt_group.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::subsys::cdmp::models::CdmpDeviceType;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::domain::system_domain::smp::DeviceMgmtGroup;
using eerie_leap::domain::canbus_com_domain::smp::NetworkMgmtGroup;

namespace {

constexpr uint32_t BUILD_NUMBER = 42;

CdmpDeviceInfo MakeDevice(uint8_t device_id) {
    return {
        .device_id = device_id,
        .uid = 0x10000000U + device_id,
        .device_type = CdmpDeviceType::LOGGER,
        .status = CdmpDeviceStatus::ONLINE,
    };
}

} // namespace

ZTEST_SUITE(smp_device_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_device_group, test_info_reports_product_versions_and_cdmp_identity) {
    auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->self = {
        .device_id = 7,
        .uid = 0xCAFEBABE,
        .device_type = CdmpDeviceType::DISPLAY,
        .status = CdmpDeviceStatus::ONLINE,
    };
    DeviceMgmtGroup group(BUILD_NUMBER, network_info);
    group.Register();
    SmpTestClient client;

    auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_equal(response.Value("family"), 2);
    zassert_equal(response.Value("product"), 4660);
    zassert_equal(response.Value("revision"), 3);
    zassert_equal(response.Value("features"), 1 << 2);
    zassert_equal(response.Value("hw"), (1U << 24) | (2U << 16) | 3U);
    zassert_equal(response.Value("sw"), (4U << 24) | (5U << 16) | 6U);
    zassert_equal(response.Value("build"), BUILD_NUMBER);
    zassert_equal(response.Value("cdmp_id"), 7);
    zassert_equal(response.Value("uid"), 0xCAFEBABE);
    zassert_equal(response.Value("device_type"), std::to_underlying(CdmpDeviceType::DISPLAY));
    zassert_equal(response.Value("status"), std::to_underlying(CdmpDeviceStatus::ONLINE));
}

ZTEST(smp_device_group, test_info_without_cdmp_has_no_identity) {
    DeviceMgmtGroup group(BUILD_NUMBER, nullptr);
    group.Register();
    SmpTestClient client;

    auto response = client.Read(SmpGroupId::DEVICE, std::to_underlying(DeviceMgmtGroup::Command::INFO));

    zassert_true(response.IsOk());
    zassert_true(response.Has("product"));
    zassert_false(response.Has("cdmp_id"));
}

ZTEST(smp_device_group, test_only_one_instance_is_registered) {
    DeviceMgmtGroup first(BUILD_NUMBER, nullptr);
    DeviceMgmtGroup second(BUILD_NUMBER, nullptr);
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
    auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->devices = {MakeDevice(2), MakeDevice(5), MakeDevice(9)};
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
    zassert_equal(response.entries[0]["type"], std::to_underlying(CdmpDeviceType::LOGGER));
    zassert_equal(response.entries[0]["status"], std::to_underlying(CdmpDeviceStatus::ONLINE));
    zassert_equal(response.entries[2]["id"], 9);
}

ZTEST(smp_network_group, test_empty_request_lists_from_the_first_device) {
    auto network_info = std::make_shared<FakeNetworkInfo>();
    network_info->devices = {MakeDevice(4)};
    NetworkMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    auto response = client.Read(SmpGroupId::NETWORK, std::to_underlying(NetworkMgmtGroup::Command::DEVICES), {});

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 1);
}

ZTEST(smp_network_group, test_a_large_network_is_paged) {
    auto network_info = std::make_shared<FakeNetworkInfo>();
    for(uint8_t id = 1; id <= 50; id++)
        network_info->devices.push_back(MakeDevice(id));
    NetworkMgmtGroup group(network_info);
    group.Register();
    SmpTestClient client;

    std::vector<uint32_t> ids;
    size_t pages = 0;
    while(ids.size() < 50) {
        auto response = client.Read(SmpGroupId::NETWORK, std::to_underlying(NetworkMgmtGroup::Command::DEVICES),
            CborMap().Put("off", ids.size()).Build());
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
