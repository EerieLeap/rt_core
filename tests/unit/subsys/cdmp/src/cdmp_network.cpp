#include <array>
#include <memory>

#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/models/messages/cdmp_discovery_response_message.h"
#include "subsys/cdmp/services/cdmp_network_service.h"
#include "subsys/cdmp/services/cdmp_service.h"

using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services;
using namespace eerie_leap::subsys::cdmp::utilities;

using eerie_leap::subsys::threading::WorkQueueThread;

ZTEST_SUITE(cdmp_network, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint32_t OWN_UID = 0x11111111;

struct NetworkFixture {
    std::shared_ptr<CdmpDevice> device = std::make_shared<CdmpDevice>(OWN_UID, CdmpDeviceType::DISPLAY);
    // Discovery responses are processed inline, so the queue is never started.
    std::shared_ptr<WorkQueueThread> work_queue = std::make_shared<WorkQueueThread>("cdmp_test_wq", 2048, 5);
    CdmpNetworkService service{std::make_shared<CdmpCanIdManager>(), device, work_queue};

    void Discover(uint8_t device_id, uint32_t uid, CdmpDeviceType type) {
        const CdmpDiscoveryResponseMessage message{.device_id = device_id, .uid = uid, .device_type = type};
        service.ProcessFrame(message.ToCanFrame());
    }
};

} // namespace

ZTEST(cdmp_network, test_snapshot_is_empty_without_peers) {
    NetworkFixture fixture;
    std::array<CdmpDeviceInfo, 4> devices{};

    zassert_equal(fixture.service.GetNetworkDevices(devices), 0);
    zassert_equal(fixture.service.GetDeviceCount(), 0);
}

ZTEST(cdmp_network, test_snapshot_copies_discovered_devices_in_id_order) {
    NetworkFixture fixture;
    fixture.Discover(5, 0x55555555, CdmpDeviceType::LOGGER);
    fixture.Discover(2, 0x22222222, CdmpDeviceType::DISPLAY);
    fixture.Discover(9, 0x99999999, CdmpDeviceType::LOGGER);

    std::array<CdmpDeviceInfo, 8> devices{};
    const size_t count = fixture.service.GetNetworkDevices(devices);

    zassert_equal(count, 3);
    zassert_equal(fixture.service.GetDeviceCount(), 3);

    zassert_equal(devices[0].device_id, 2);
    zassert_equal(devices[0].uid, 0x22222222);
    zassert_equal(devices[0].device_type, CdmpDeviceType::DISPLAY);
    zassert_equal(devices[0].status, CdmpDeviceStatus::ONLINE);
    zassert_equal(devices[1].device_id, 5);
    zassert_equal(devices[2].device_id, 9);
    zassert_equal(devices[2].device_type, CdmpDeviceType::LOGGER);
}

ZTEST(cdmp_network, test_snapshot_is_limited_to_the_buffer) {
    NetworkFixture fixture;
    fixture.Discover(1, 0xA1, CdmpDeviceType::LOGGER);
    fixture.Discover(2, 0xA2, CdmpDeviceType::LOGGER);
    fixture.Discover(3, 0xA3, CdmpDeviceType::LOGGER);

    std::array<CdmpDeviceInfo, 2> devices{};

    zassert_equal(fixture.service.GetNetworkDevices(devices), 2);
    zassert_equal(fixture.service.GetDeviceCount(), 3);
}

ZTEST(cdmp_network, test_removed_device_leaves_the_snapshot) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, CdmpDeviceType::LOGGER);
    fixture.Discover(6, 0x66666666, CdmpDeviceType::LOGGER);

    fixture.service.RemoveDevice(4);

    std::array<CdmpDeviceInfo, 4> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].device_id, 6);
}

ZTEST(cdmp_network, test_uid_conflict_is_not_added) {
    NetworkFixture fixture;

    fixture.Discover(3, OWN_UID, CdmpDeviceType::LOGGER);

    std::array<CdmpDeviceInfo, 4> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 0);
    zassert_equal(fixture.device->GetStatus(), CdmpDeviceStatus::ERROR,
        "A peer with this device's UID puts the device into ERROR");
}

ZTEST_SUITE(cdmp_service, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_service, test_status_changed_handler_sees_transitions) {
    CdmpService service(CdmpDeviceType::DISPLAY, OWN_UID);

    CdmpDeviceStatus seen_old = CdmpDeviceStatus::ONLINE;
    CdmpDeviceStatus seen_new = CdmpDeviceStatus::ONLINE;
    int calls = 0;

    int handler_id = service.RegisterStatusChangedHandler(
        [&](CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
            seen_old = old_status;
            seen_new = new_status;
            calls++;
        });

    service.GetDevice()->EnterError();

    zassert_equal(calls, 1);
    zassert_equal(seen_old, CdmpDeviceStatus::OFFLINE);
    zassert_equal(seen_new, CdmpDeviceStatus::ERROR);

    service.UnregisterStatusChangedHandler(handler_id);
    service.GetDevice()->Reset();

    zassert_equal(calls, 1, "An unregistered handler must not be called");
}

ZTEST(cdmp_service, test_network_snapshot_starts_empty) {
    CdmpService service(CdmpDeviceType::DISPLAY, OWN_UID);
    std::array<CdmpDeviceInfo, 4> devices{};

    zassert_equal(service.GetNetworkDevices(devices), 0);
}
