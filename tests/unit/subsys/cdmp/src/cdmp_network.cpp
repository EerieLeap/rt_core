#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "cdmp_test_features.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/cdmp/models/messages/cdmp_discovery_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_discovery_response_message.h"
#include "subsys/cdmp/models/messages/cdmp_heartbeat_message.h"
#include "subsys/cdmp/models/messages/cdmp_id_claim_request_message.h"
#include "subsys/cdmp/models/messages/cdmp_id_claim_response_message.h"
#include "subsys/cdmp/services/cdmp_network_service.h"
#include "subsys/cdmp/services/cdmp_service.h"
#include "subsys/threading/work_queue_thread.h"

using eerie_leap::tests::DISPLAY_FEATURES;
using eerie_leap::tests::LOGGER_FEATURES;

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services;
using namespace eerie_leap::subsys::cdmp::utilities;

using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr uint32_t OWN_UID = 0x11111111;
constexpr int FRAME_TIMEOUT_MS = 1000;
// Well past the time a frame takes through the loopback controller.
constexpr int NO_FRAME_TIMEOUT_MS = 100;
// Three discovery backoffs (800 ms) plus ten unanswered claims (500 ms), with room to spare.
constexpr int STARTUP_TIMEOUT_MS = 3000;

using Frame = std::vector<uint8_t>;

const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the fixture teardown, so the controller is stopped separately.
void ResetLoopbackDevice(void* /*fixture*/) {
    can_stop(LoopbackDevice());
}

Frame DiscoveryResponseFrame(uint8_t device_id, uint32_t uid, uint16_t features) {
    return CdmpDiscoveryResponseMessage{ .device_id = device_id, .uid = uid, .features = features }.ToCanFrame();
}

Frame ClaimFrame(uint8_t device_id, uint32_t uid, uint8_t protocol_version = CdmpDevice::GetCurrentProtocolVersion()) {

    return CdmpIdClaimRequestMessage{
        .claiming_device_id = device_id, .uid = uid, .protocol_version = protocol_version
    }
        .ToCanFrame();
}

Frame ClaimResponseFrame(uint8_t responding_device_id, uint8_t claiming_device_id, CdmpIdClaimResult result) {
    return CdmpIdClaimResponseMessage{ .responding_device_id = responding_device_id,
        .claiming_device_id = claiming_device_id,
        .claiming_device_uid = OWN_UID,
        .result = result }
        .ToCanFrame();
}

// Management frames seen on the loopback bus, in order.
class BusFrames {
private:
    k_sem received_{};
    k_mutex lock_{};
    std::vector<Frame> frames_;

public:
    BusFrames() {
        k_sem_init(&received_, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&lock_);
    }

    void Add(const CanFrame& frame) {
        k_mutex_lock(&lock_, K_FOREVER);
        frames_.emplace_back(frame.data.begin(), frame.data.end());
        k_mutex_unlock(&lock_);

        k_sem_give(&received_);
    }

    std::optional<Frame> Wait(int timeout_ms = FRAME_TIMEOUT_MS) {
        if(k_sem_take(&received_, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        Frame frame = std::move(frames_.front());
        frames_.erase(frames_.begin());
        k_mutex_unlock(&lock_);

        return frame;
    }

    /** @brief Skips frames of other types. */
    std::optional<Frame> Wait(CdmpManagementMessageType type, int timeout_ms = FRAME_TIMEOUT_MS) {
        const int64_t deadline = k_uptime_get() + timeout_ms;

        for(int64_t remaining = timeout_ms; remaining > 0; remaining = deadline - k_uptime_get()) {
            auto frame = Wait(static_cast<int>(remaining));
            if(!frame.has_value())
                break;

            if(!frame->empty() && frame->front() == std::to_underlying(type))
                return frame;
        }

        return std::nullopt;
    }
};

struct DeviceEventRecord {
    CdmpDeviceEvent event;
    CdmpDeviceInfo device;
};

struct HeartbeatRecord {
    CdmpHeartbeatMessage heartbeat;
    size_t device_events_before; // Device events reported before this heartbeat was passed on.
};

struct NetworkFixture {
    std::shared_ptr<CdmpDevice> device = std::make_shared<CdmpDevice>(OWN_UID, DISPLAY_FEATURES);
    std::shared_ptr<WorkQueueThread> work_queue = std::make_shared<WorkQueueThread>(
        "cdmp_test_wq",
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE,
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_PRIORITY
    );
    std::shared_ptr<CdmpCanIdManager> can_id_manager = std::make_shared<CdmpCanIdManager>();

    // Before the service, which reports into them for as long as it exists.
    std::vector<DeviceEventRecord> events;
    std::vector<HeartbeatRecord> heartbeats;

    // Unless a test starts it, the service has no CAN bus and processes frames inline on the test
    // thread, so this unit never answers a claim.
    CdmpNetworkService service{ can_id_manager, device, work_queue };

    std::shared_ptr<CanbusProxy> canbus;
    int bus_handler_id = -1;
    BusFrames bus_frames;

    NetworkFixture() {
        service.RegisterDeviceEventHandler([this](CdmpDeviceEvent event, const CdmpDeviceInfo& info) {
            events.push_back({ .event = event, .device = info });
        });
        service.RegisterHeartbeatHandler([this](const CdmpHeartbeatMessage& heartbeat) {
            heartbeats.push_back({ .heartbeat = heartbeat, .device_events_before = events.size() });
        });
    }

    NetworkFixture(const NetworkFixture&) = delete;
    NetworkFixture& operator=(const NetworkFixture&) = delete;
    NetworkFixture(NetworkFixture&&) = delete;
    NetworkFixture& operator=(NetworkFixture&&) = delete;

    ~NetworkFixture() {
        // Get() instead of operator->, which throws when the proxy holds no instance.
        if(canbus != nullptr) {
            if(Canbus* live_bus = canbus->Get(); live_bus != nullptr)
                live_bus->RemoveFrameReceivedHandler(bus_handler_id);
        }

        // Queued work must run while the service exists; after the stop it does nothing.
        service.Stop();
        work_queue->Stop();
    }

    void Start() {
        zassert_true(work_queue->Initialize());
        zassert_true(service.Initialize());
        zassert_true(service.Start());
    }

    // Records every management frame on a loopback controller, and lets the service send on it.
    void AttachLoopbackBus() {
        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(LoopbackDevice(), CanbusType::CLASSICAL_CAN, 500000, 0, CAN_MODE_LOOPBACK)
        );
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        // The same filter the service registers, so it is shared rather than an overlap.
        const CanId management_id = can_id_manager->GetManagementCanId();
        bus_handler_id = (*canbus)->RegisterFrameReceivedHandler(
            can_id_manager->GetReceiveFilter(management_id), [this, management_id](const CanFrame& frame) {
                if(frame.id == management_id.id)
                    bus_frames.Add(frame);
            }
        );
        zassert_true(bus_handler_id > 0, "Frame handler rejected: %d", bus_handler_id);

        service.Configure(canbus);
    }

    // The loopback controller would hand the service its own frames, so it gets the bus after
    // Start(), without a receive handler; the test hands frames over itself.
    void StartWithoutReceiving() {
        Start();
        AttachLoopbackBus();
    }

    void GoOnline(uint8_t device_id) {
        StartWithoutReceiving();

        device->SetDeviceId(device_id);
        device->GoOnline(true);
        // As the validation task does right away: alone, this unit holds the lowest ID.
        service.ProcessPeriodicTasks();
    }

    // As the receive handler of a running service hands a frame over.
    void Receive(Frame frame) {
        work_queue->Run([this, frame = std::move(frame)] {
            service.ProcessFrame(frame);
        });
    }

    void SendAsPeer(const Frame& frame) {
        const int result = (*canbus)->SendFrame(can_id_manager->GetManagementCanId(), frame, K_MSEC(100));
        zassert_equal(result, 0, "SendFrame failed: %d", result);
    }

    /** @brief Returns once the work queued so far has run. */
    void DrainQueue() {
        k_sem done{};
        k_sem_init(&done, 0, 1);
        work_queue->Run([&done] {
            k_sem_give(&done);
        });
        zassert_equal(k_sem_take(&done, K_MSEC(FRAME_TIMEOUT_MS)), 0, "The work queue is stuck");
    }

    bool WaitForStatus(CdmpDeviceStatus status, int timeout_ms = STARTUP_TIMEOUT_MS) const {
        const int64_t deadline = k_uptime_get() + timeout_ms;

        while(device->GetStatus() != status) {
            if(k_uptime_get() >= deadline)
                return false;

            k_msleep(10);
        }

        return true;
    }

    std::optional<CdmpIdClaimRequestMessage> WaitForClaim(int timeout_ms = STARTUP_TIMEOUT_MS) {
        auto frame = bus_frames.Wait(CdmpManagementMessageType::ID_CLAIM, timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        return CdmpIdClaimRequestMessage::FromCanFrame(*frame);
    }

    void Discover(uint8_t device_id, uint32_t uid, uint16_t features) {
        service.ProcessFrame(DiscoveryResponseFrame(device_id, uid, features));
    }

    void Claim(uint8_t device_id, uint32_t uid, uint8_t protocol_version = CdmpDevice::GetCurrentProtocolVersion()) {

        service.ProcessFrame(ClaimFrame(device_id, uid, protocol_version));
    }

    // Through the frame codec, as the heartbeat service hands it over.
    void Heartbeat(uint8_t device_id, uint16_t features = 0) {
        const CdmpHeartbeatMessage message{ .device_id = device_id,
            .health_status = CdmpHealthStatus::OK,
            .sequence_number = 0,
            .features = features,
            .state_digest = 0 };
        service.UpdateDeviceFromHeartbeat(CdmpHeartbeatMessage::FromCanFrame(message.ToCanFrame()));
    }
};

} // namespace

ZTEST_SUITE(cdmp_network, NULL, NULL, NULL, ResetLoopbackDevice, NULL);

ZTEST(cdmp_network, test_snapshot_is_empty_without_peers) {
    const NetworkFixture fixture;
    std::array<CdmpDeviceInfo, 4> devices{};

    zassert_equal(fixture.service.GetNetworkDevices(devices), 0);
    zassert_equal(fixture.service.GetDeviceCount(), 0);
}

ZTEST(cdmp_network, test_snapshot_copies_discovered_devices_in_id_order) {
    NetworkFixture fixture;
    fixture.Discover(5, 0x55555555, LOGGER_FEATURES);
    fixture.Discover(2, 0x22222222, DISPLAY_FEATURES);
    fixture.Discover(9, 0x99999999, LOGGER_FEATURES);

    std::array<CdmpDeviceInfo, 8> devices{};
    const size_t count = fixture.service.GetNetworkDevices(devices);

    zassert_equal(count, 3);
    zassert_equal(fixture.service.GetDeviceCount(), 3);

    zassert_equal(devices[0].device_id, 2);
    zassert_equal(devices[0].uid, 0x22222222);
    zassert_equal(devices[0].features, DISPLAY_FEATURES);
    zassert_equal(devices[0].status, CdmpDeviceStatus::ONLINE);
    zassert_equal(devices[1].device_id, 5);
    zassert_equal(devices[2].device_id, 9);
    zassert_equal(devices[2].features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_snapshot_is_limited_to_the_buffer) {
    NetworkFixture fixture;
    fixture.Discover(1, 0xA1, LOGGER_FEATURES);
    fixture.Discover(2, 0xA2, LOGGER_FEATURES);
    fixture.Discover(3, 0xA3, LOGGER_FEATURES);

    std::array<CdmpDeviceInfo, 2> devices{};

    zassert_equal(fixture.service.GetNetworkDevices(devices), 2);
    zassert_equal(fixture.service.GetDeviceCount(), 3);
}

ZTEST(cdmp_network, test_removed_device_leaves_the_snapshot) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.Discover(6, 0x66666666, LOGGER_FEATURES);

    fixture.service.RemoveDevice(4);

    std::array<CdmpDeviceInfo, 4> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].device_id, 6);
}

ZTEST(cdmp_network, test_uid_conflict_is_not_added) {
    NetworkFixture fixture;

    fixture.Discover(3, OWN_UID, LOGGER_FEATURES);

    std::array<CdmpDeviceInfo, 4> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 0);
    zassert_equal(
        fixture.device->GetStatus(), CdmpDeviceStatus::ERROR, "A peer with this device's UID puts the device into ERROR"
    );
}

ZTEST(cdmp_network, test_discovered_device_reports_joined) {
    NetworkFixture fixture;

    fixture.Discover(5, 0x55555555, LOGGER_FEATURES);
    fixture.Discover(5, 0x55555555, LOGGER_FEATURES);

    zassert_equal(fixture.events.size(), 1, "Only the first response adds the device");
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.device_id, 5);
    zassert_equal(fixture.events[0].device.uid, 0x55555555);
    zassert_equal(fixture.events[0].device.features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_unit_that_is_not_lowest_learns_claimant_from_first_heartbeat) {
    NetworkFixture fixture;
    // Device 1 is the lowest ID and answers the claim; this unit, device 2, only listens.
    fixture.device->SetDeviceId(2);
    fixture.Discover(1, 0x10101010, LOGGER_FEATURES);
    fixture.events.clear();

    fixture.Claim(3, 0x33333333);

    zassert_equal(fixture.service.GetDeviceCount(), 1, "A claim alone does not add the claimant");
    zassert_true(fixture.events.empty());

    fixture.Heartbeat(3, LOGGER_FEATURES);

    std::array<CdmpDeviceInfo, 4> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 2);
    zassert_equal(devices[1].device_id, 3);
    zassert_equal(devices[1].uid, 0x33333333, "The UID comes from the claim");
    zassert_equal(devices[1].features, LOGGER_FEATURES);
    zassert_equal(devices[1].status, CdmpDeviceStatus::ONLINE);
    zassert_true(fixture.service.IsDeviceOnline(3));

    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.device_id, 3);
    zassert_equal(fixture.events[0].device.uid, 0x33333333);

    zassert_equal(fixture.heartbeats.size(), 1);
    zassert_equal(fixture.heartbeats[0].heartbeat.device_id, 3);
    zassert_equal(fixture.heartbeats[0].device_events_before, 1, "JOINED comes before the heartbeat");
}

ZTEST(cdmp_network, test_second_heartbeat_does_not_join_again) {
    NetworkFixture fixture;
    fixture.Claim(3, 0x33333333);

    fixture.Heartbeat(3);
    fixture.Heartbeat(3);

    zassert_equal(fixture.service.GetDeviceCount(), 1);
    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.heartbeats.size(), 2);
}

ZTEST(cdmp_network, test_heartbeat_without_claim_is_ignored) {
    NetworkFixture fixture;

    fixture.Heartbeat(4);

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_true(fixture.events.empty());
    zassert_true(fixture.heartbeats.empty(), "Only heartbeats of devices in the table are passed on");
}

// One wait past the timeout covers both, since it costs real time on boards without icount.
ZTEST(cdmp_network, test_silence_past_the_heartbeat_timeout_expires_claims_and_devices) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.Claim(3, 0x33333333);
    fixture.events.clear();

    k_msleep(CdmpConstants::HEARTBEAT_TIMEOUT_MS + 1);
    fixture.service.ProcessPeriodicTasks();

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_equal(fixture.events.size(), 1, "The validation removes the silent device");
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::LEFT);
    zassert_equal(fixture.events[0].device.device_id, 4);
    zassert_equal(fixture.events[0].device.uid, 0x44444444);

    fixture.Heartbeat(3);

    zassert_equal(fixture.service.GetDeviceCount(), 0, "The expired claim does not join");
    zassert_equal(fixture.events.size(), 1);
    zassert_true(fixture.heartbeats.empty());
}

ZTEST(cdmp_network, test_full_pending_claims_replace_the_oldest) {
    NetworkFixture fixture;
    constexpr uint8_t CLAIMS = CdmpConstants::PENDING_ID_CLAIMS_MAX + 1;

    for(uint8_t id = 1; id <= CLAIMS; id++)
        fixture.Claim(id, 0xC0000000 | id);
    for(uint8_t id = 1; id <= CLAIMS; id++)
        fixture.Heartbeat(id);

    zassert_equal(fixture.service.GetDeviceCount(), CLAIMS - 1);
    zassert_false(fixture.service.IsDeviceOnline(1), "The first claim made room for the last");
    zassert_true(fixture.service.IsDeviceOnline(2));
    zassert_true(fixture.service.IsDeviceOnline(CLAIMS));
}

ZTEST(cdmp_network, test_incompatible_claim_is_not_recorded) {
    NetworkFixture fixture;

    fixture.Claim(3, 0x33333333, 0x20);
    fixture.Heartbeat(3);

    zassert_equal(fixture.service.GetDeviceCount(), 0, "The lowest ID refuses the claim");
    zassert_true(fixture.events.empty());
}

ZTEST(cdmp_network, test_own_claim_is_not_recorded) {
    NetworkFixture fixture;

    fixture.Claim(3, OWN_UID);
    fixture.Heartbeat(3);

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_true(fixture.events.empty());
}

ZTEST(cdmp_network, test_same_uid_claim_reports_restarted_once) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.events.clear();

    // A claimant repeats its claim until it is answered.
    fixture.Claim(4, 0x44444444);
    fixture.Claim(4, 0x44444444);

    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::RESTARTED);
    zassert_equal(fixture.events[0].device.device_id, 4);
    zassert_equal(fixture.events[0].device.uid, 0x44444444);
    zassert_equal(fixture.service.GetDeviceCount(), 1, "A restarted device stays in the table");

    fixture.Heartbeat(4);

    zassert_equal(fixture.events.size(), 1, "Its first heartbeat is not a new join");
    zassert_equal(fixture.heartbeats.size(), 1);

    fixture.Claim(4, 0x44444444);

    zassert_equal(fixture.events.size(), 2, "A claim after a heartbeat is the next restart");
    zassert_equal(fixture.events[1].event, CdmpDeviceEvent::RESTARTED);
}

ZTEST(cdmp_network, test_claim_with_another_uid_for_known_id_changes_nothing) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.events.clear();

    fixture.Claim(4, 0x55555555);
    fixture.Heartbeat(4, LOGGER_FEATURES);

    zassert_true(fixture.events.empty(), "The owner rejects a conflicting claim");

    std::array<CdmpDeviceInfo, 2> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].uid, 0x44444444);
    zassert_equal(devices[0].features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_known_uid_claiming_another_id_leaves_the_old_one) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.events.clear();

    fixture.Claim(6, 0x44444444);

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::LEFT);
    zassert_equal(fixture.events[0].device.device_id, 4);

    fixture.Heartbeat(6);

    zassert_equal(fixture.events.size(), 2);
    zassert_equal(fixture.events[1].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[1].device.device_id, 6);
    zassert_equal(fixture.events[1].device.uid, 0x44444444);
}

ZTEST(cdmp_network, test_discovered_uid_under_another_id_replaces_the_old_entry) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.events.clear();

    // It restarted under ID 6, and this unit missed the claim.
    fixture.Discover(6, 0x44444444, LOGGER_FEATURES);

    zassert_equal(fixture.service.GetDeviceCount(), 1);
    zassert_equal(fixture.events.size(), 2);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::LEFT);
    zassert_equal(fixture.events[0].device.device_id, 4);
    zassert_equal(fixture.events[1].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[1].device.device_id, 6);
}

ZTEST(cdmp_network, test_removed_device_reports_left) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.events.clear();

    fixture.service.RemoveDevice(4);
    fixture.service.RemoveDevice(4);

    zassert_equal(fixture.events.size(), 1, "Removing an unknown device reports nothing");
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::LEFT);
    zassert_equal(fixture.events[0].device.device_id, 4);
    zassert_equal(fixture.events[0].device.uid, 0x44444444);
    zassert_equal(fixture.events[0].device.features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_heartbeat_handler_receives_the_full_message) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);

    const CdmpHeartbeatMessage sent{ .device_id = 4,
        .health_status = CdmpHealthStatus::WARNING,
        .sequence_number = 7,
        .features = 0x0304,
        .state_digest = 0xA5 };
    fixture.service.UpdateDeviceFromHeartbeat(sent);

    zassert_equal(fixture.heartbeats.size(), 1);
    const CdmpHeartbeatMessage& received = fixture.heartbeats[0].heartbeat;
    zassert_equal(received.device_id, 4);
    zassert_equal(received.health_status, CdmpHealthStatus::WARNING);
    zassert_equal(received.sequence_number, 7);
    zassert_equal(received.features, 0x0304);
    zassert_equal(received.state_digest, 0xA5);
}

ZTEST(cdmp_network, test_unregistered_handlers_are_not_called) {
    NetworkFixture fixture;
    int device_events = 0;
    int heartbeats = 0;

    const int event_handler_id = fixture.service.RegisterDeviceEventHandler([&](CdmpDeviceEvent,
                                                                                const CdmpDeviceInfo&) {
        device_events++;
    });
    const int heartbeat_handler_id = fixture.service.RegisterHeartbeatHandler([&](const CdmpHeartbeatMessage&) {
        heartbeats++;
    });

    zassert_not_equal(event_handler_id, heartbeat_handler_id);

    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    fixture.Heartbeat(4);

    zassert_equal(device_events, 1);
    zassert_equal(heartbeats, 1);

    fixture.service.UnregisterDeviceEventHandler(event_handler_id);
    fixture.service.UnregisterHeartbeatHandler(heartbeat_handler_id);

    fixture.service.RemoveDevice(4);
    fixture.Discover(5, 0x55555555, LOGGER_FEATURES);
    fixture.Heartbeat(5);

    zassert_equal(device_events, 1);
    zassert_equal(heartbeats, 1);
    zassert_equal(fixture.events.size(), 3, "The other handlers stay registered");
    zassert_equal(fixture.heartbeats.size(), 2);
}

ZTEST(cdmp_network, test_claim_and_heartbeat_for_own_id_are_ignored_while_online) {
    NetworkFixture fixture;
    fixture.GoOnline(2);

    fixture.Claim(2, 0x22222222);

    auto frame = fixture.bus_frames.Wait(CdmpManagementMessageType::ID_CLAIM_RESPONSE);
    zassert_true(frame.has_value(), "The owner answers a claim on its ID");
    const auto response = CdmpIdClaimResponseMessage::FromCanFrame(*frame);
    zassert_equal(response.result, CdmpIdClaimResult::REJECT);
    zassert_equal(response.responding_device_id, 2);
    zassert_equal(response.claiming_device_id, 2);

    fixture.Heartbeat(2);

    zassert_false(
        fixture.bus_frames.Wait(NO_FRAME_TIMEOUT_MS).has_value(),
        "This unit's own ID is no unknown device to rediscover"
    );
    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_true(fixture.events.empty());
    zassert_true(fixture.heartbeats.empty());
}

ZTEST(cdmp_network, test_discovery_response_with_reserved_or_own_id_is_ignored) {
    NetworkFixture fixture;
    fixture.GoOnline(2);

    fixture.Discover(CdmpDevice::DEVICE_ID_UNASSIGNED, 0x0A0A0A0A, LOGGER_FEATURES);
    fixture.Discover(CdmpDevice::DEVICE_ID_BROADCAST, 0x0B0B0B0B, LOGGER_FEATURES);
    fixture.Discover(2, 0x22222222, LOGGER_FEATURES);

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_true(fixture.events.empty());
    zassert_equal(fixture.device->GetStatus(), CdmpDeviceStatus::ONLINE);
}

ZTEST(cdmp_network, test_lowest_id_accepts_claim_and_learns_claimant_from_heartbeat) {
    NetworkFixture fixture;
    fixture.GoOnline(1);

    fixture.Claim(3, 0x33333333);

    auto frame = fixture.bus_frames.Wait(CdmpManagementMessageType::ID_CLAIM_RESPONSE);
    zassert_true(frame.has_value(), "The lowest ID answers the claim");
    const auto response = CdmpIdClaimResponseMessage::FromCanFrame(*frame);
    zassert_equal(response.result, CdmpIdClaimResult::ACCEPT);
    zassert_equal(response.responding_device_id, 1);
    zassert_equal(response.claiming_device_id, 3);

    zassert_equal(fixture.service.GetDeviceCount(), 0, "Accepting does not add the claimant");
    zassert_true(fixture.events.empty());

    fixture.Heartbeat(3);

    zassert_equal(fixture.service.GetDeviceCount(), 1);
    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.device_id, 3);
    zassert_equal(fixture.events[0].device.uid, 0x33333333);
}

ZTEST(cdmp_network, test_heartbeat_from_removed_device_requests_rediscovery) {
    NetworkFixture fixture;
    fixture.GoOnline(2);
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);
    // As after the heartbeat timeout, for example while its cable was unplugged.
    fixture.service.RemoveDevice(4);
    fixture.events.clear();

    fixture.Heartbeat(4);

    auto frame = fixture.bus_frames.Wait(CdmpManagementMessageType::DISCOVERY_REQUEST);
    zassert_true(frame.has_value(), "A heartbeat from an unknown device requests discovery");
    zassert_equal(CdmpDiscoveryRequestMessage::FromCanFrame(*frame).uid, OWN_UID);
    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_true(fixture.events.empty());
    zassert_true(fixture.heartbeats.empty(), "Not passed on before the device is back in the table");

    fixture.Heartbeat(4);

    zassert_false(
        fixture.bus_frames.Wait(NO_FRAME_TIMEOUT_MS).has_value(), "At most one request per heartbeat interval"
    );

    // Its answer to the request.
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);

    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.device_id, 4);

    fixture.Heartbeat(4);

    zassert_equal(fixture.heartbeats.size(), 1);
}

ZTEST(cdmp_network, test_discovery_answer_and_pending_claim_steer_the_id_claim) {
    NetworkFixture fixture;
    fixture.StartWithoutReceiving();

    fixture.device->StartDiscovery();

    zassert_true(fixture.bus_frames.Wait(CdmpManagementMessageType::DISCOVERY_REQUEST).has_value());

    // Received while discovery waits: device 1 answers, and another unit claims ID 2.
    fixture.Receive(DiscoveryResponseFrame(1, 0x10101010, LOGGER_FEATURES));
    fixture.Receive(ClaimFrame(2, 0x20202020));

    auto claim = fixture.WaitForClaim();
    zassert_true(claim.has_value());
    zassert_equal(claim->claiming_device_id, 3, "ID 1 is in the table and ID 2 has a pending claim");
    zassert_equal(claim->uid, OWN_UID);

    zassert_true(fixture.WaitForStatus(CdmpDeviceStatus::ONLINE), "An unopposed claim goes online");
    zassert_equal(fixture.device->GetDeviceId(), 3);

    fixture.DrainQueue();
    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.device_id, 1);
}

ZTEST(cdmp_network, test_rejected_claim_moves_to_the_next_free_id) {
    NetworkFixture fixture;
    fixture.StartWithoutReceiving();

    fixture.device->StartDiscovery();

    auto claim = fixture.WaitForClaim();
    zassert_true(claim.has_value(), "Nobody answers discovery, so the unit claims an ID");
    zassert_equal(claim->claiming_device_id, 1);

    // The owner of ID 1 objects; the lowest ID's acceptance of the same claim does not outweigh that.
    fixture.Receive(ClaimResponseFrame(1, 1, CdmpIdClaimResult::REJECT));
    fixture.Receive(ClaimResponseFrame(1, 1, CdmpIdClaimResult::ACCEPT));

    do {
        claim = fixture.WaitForClaim(FRAME_TIMEOUT_MS);
        zassert_true(claim.has_value(), "The claim goes on after the reject");
    } while(claim->claiming_device_id == 1);

    zassert_equal(claim->claiming_device_id, 2);

    fixture.Receive(ClaimResponseFrame(1, 2, CdmpIdClaimResult::ACCEPT));

    zassert_true(fixture.WaitForStatus(CdmpDeviceStatus::ONLINE));
    zassert_equal(fixture.device->GetDeviceId(), 2);
}

ZTEST(cdmp_network, test_incompatible_version_answer_ends_the_claim) {
    NetworkFixture fixture;
    fixture.StartWithoutReceiving();

    fixture.device->StartDiscovery();

    auto claim = fixture.WaitForClaim();
    zassert_true(claim.has_value());

    fixture.Receive(ClaimResponseFrame(1, claim->claiming_device_id, CdmpIdClaimResult::VERSION_INCOMPATIBLE));

    zassert_true(fixture.WaitForStatus(CdmpDeviceStatus::VERSION_MISMATCH));
    fixture.DrainQueue();
    // The mismatch stops the service from its work queue, which must not wait on itself.
    zassert_equal(fixture.service.GetState(), ServiceState::STOPPED);
    zassert_false(
        fixture.bus_frames.Wait(CdmpManagementMessageType::ID_CLAIM, NO_FRAME_TIMEOUT_MS).has_value(),
        "No claim follows the mismatch"
    );
}

ZTEST(cdmp_network, test_frames_queued_before_a_stop_are_dropped) {
    NetworkFixture fixture;
    // With the bus attached first, frames arrive through the receive handler. The unit stays
    // offline, so it sends nothing of its own.
    fixture.AttachLoopbackBus();
    fixture.Start();

    fixture.SendAsPeer(DiscoveryResponseFrame(5, 0x55555555, LOGGER_FEATURES));
    zassert_true(fixture.bus_frames.Wait().has_value());
    fixture.DrainQueue();

    zassert_equal(fixture.service.GetDeviceCount(), 1);
    zassert_equal(fixture.events.size(), 1);

    // Hold the queue, so the next frame waits behind it until after the stop.
    k_sem release{};
    k_sem_init(&release, 0, 1);
    fixture.work_queue->Run([&release] {
        k_sem_take(&release, K_FOREVER);
    });

    fixture.SendAsPeer(DiscoveryResponseFrame(6, 0x66666666, LOGGER_FEATURES));
    // Both handlers see a frame in one dispatch, so the service's has queued it by now.
    zassert_true(fixture.bus_frames.Wait().has_value());

    zassert_true(fixture.service.Stop());
    k_sem_give(&release);
    fixture.DrainQueue();

    zassert_equal(fixture.service.GetDeviceCount(), 0);
    zassert_equal(fixture.events.size(), 1, "Device 6 arrived before the stop and must not join after it");

    // A start forgets whatever reached the table while stopped.
    fixture.Discover(7, 0x77777777, LOGGER_FEATURES);
    zassert_true(fixture.service.Start());

    zassert_equal(fixture.service.GetDeviceCount(), 0);
}

ZTEST(cdmp_network, test_heartbeat_with_no_features_clears_the_devices_mask) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);

    fixture.Heartbeat(4, 0);

    std::array<CdmpDeviceInfo, 2> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].features, 0, "A unit that clears LOGGING says so in its next heartbeat");

    fixture.Heartbeat(4, LOGGER_FEATURES);

    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_claimant_joins_with_the_features_of_its_first_heartbeat) {
    NetworkFixture fixture;
    fixture.device->SetDeviceId(2);
    fixture.Discover(1, 0x10101010, DISPLAY_FEATURES);
    fixture.events.clear();

    fixture.Claim(3, 0x33333333);
    fixture.Heartbeat(3, LOGGER_FEATURES | DISPLAY_FEATURES);

    zassert_equal(fixture.events.size(), 1);
    zassert_equal(fixture.events[0].event, CdmpDeviceEvent::JOINED);
    zassert_equal(fixture.events[0].device.features, LOGGER_FEATURES | DISPLAY_FEATURES);
}

ZTEST(cdmp_network, test_heartbeat_reserved_bytes_are_ignored) {
    NetworkFixture fixture;
    fixture.Discover(4, 0x44444444, LOGGER_FEATURES);

    auto frame = CdmpHeartbeatMessage{ .device_id = 4,
        .health_status = CdmpHealthStatus::OK,
        .sequence_number = 0,
        .features = LOGGER_FEATURES,
        .state_digest = 0 }
                     .ToCanFrame();
    frame[5] = 0xFF;
    frame[6] = 0xFF;
    fixture.service.UpdateDeviceFromHeartbeat(CdmpHeartbeatMessage::FromCanFrame(frame));

    std::array<CdmpDeviceInfo, 2> devices{};
    zassert_equal(fixture.service.GetNetworkDevices(devices), 1);
    zassert_equal(devices[0].features, LOGGER_FEATURES);
}

ZTEST(cdmp_network, test_short_discovery_response_adds_no_device) {
    NetworkFixture fixture;
    auto frame = DiscoveryResponseFrame(5, 0x55555555, LOGGER_FEATURES);
    frame.pop_back();

    fixture.service.ProcessFrame(frame);

    zassert_equal(fixture.service.GetDeviceCount(), 0, "A discovery response needs all 8 bytes");
}

ZTEST(cdmp_network, test_id_claim_sends_byte_6_as_zero_and_ignores_it) {
    const auto frame = CdmpIdClaimRequestMessage{ .claiming_device_id = 3,
        .uid = 0x33333333,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion() }
                           .ToCanFrame();

    zassert_equal(frame.size(), 8);
    zassert_equal(frame[6], 0, "Byte 6 is reserved");

    auto received = frame;
    received[6] = 0xFF;
    const auto message = CdmpIdClaimRequestMessage::FromCanFrame(received);

    zassert_equal(message.claiming_device_id, 3);
    zassert_equal(message.uid, 0x33333333);
    zassert_equal(message.protocol_version, CdmpDevice::GetCurrentProtocolVersion());
}

ZTEST_SUITE(cdmp_discovery_response_message, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_discovery_response_message, test_features_are_in_bytes_6_and_7) {
    const auto frame = DiscoveryResponseFrame(5, 0x01020304, 0x2010);

    zassert_equal(frame.size(), CdmpDiscoveryResponseMessage::FRAME_SIZE);
    zassert_equal(frame[6], 0x10, "Features are LSB first");
    zassert_equal(frame[7], 0x20);

    const auto message = CdmpDiscoveryResponseMessage::FromCanFrame(frame);

    zassert_equal(message.device_id, 5);
    zassert_equal(message.uid, 0x01020304);
    zassert_equal(message.features, 0x2010);
}

ZTEST_SUITE(cdmp_heartbeat_message, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_heartbeat_message, test_round_trips_with_state_digest) {
    const CdmpHeartbeatMessage sent{ .device_id = 9,
        .health_status = CdmpHealthStatus::WARNING,
        .sequence_number = 200,
        .features = 0x2010,
        .state_digest = 0xC3 };

    const auto frame = sent.ToCanFrame();

    zassert_equal(frame.size(), CdmpHeartbeatMessage::FRAME_SIZE);
    zassert_equal(frame[3], 0x10, "Features are LSB first");
    zassert_equal(frame[4], 0x20);
    zassert_equal(frame[5], 0, "Bytes 5 and 6 are reserved");
    zassert_equal(frame[6], 0);
    zassert_equal(frame[7], 0xC3, "Byte 7 is the state digest");

    const auto received = CdmpHeartbeatMessage::FromCanFrame(frame);

    zassert_equal(received.device_id, 9);
    zassert_equal(received.health_status, CdmpHealthStatus::WARNING);
    zassert_equal(received.sequence_number, 200);
    zassert_equal(received.features, 0x2010);
    zassert_equal(received.state_digest, 0xC3);
}

ZTEST(cdmp_heartbeat_message, test_short_frame_is_rejected) {
    const std::array<uint8_t, 7> frame{ 9, 0x00, 1, 0x01, 0x00, 0x00, 0x00 };
    bool rejected = false;

    try {
        (void)CdmpHeartbeatMessage::FromCanFrame(frame);
    } catch(const std::invalid_argument&) {
        rejected = true;
    }

    zassert_true(rejected, "A heartbeat without byte 7 is malformed");
}

ZTEST_SUITE(cdmp_service, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_service, test_status_changed_handler_sees_transitions) {
    CdmpService service(DISPLAY_FEATURES, OWN_UID);

    CdmpDeviceStatus seen_old = CdmpDeviceStatus::ONLINE;
    CdmpDeviceStatus seen_new = CdmpDeviceStatus::ONLINE;
    int calls = 0;

    const int handler_id = service.RegisterStatusChangedHandler([&](CdmpDeviceStatus old_status,
                                                                    CdmpDeviceStatus new_status) {
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
    const CdmpService service(DISPLAY_FEATURES, OWN_UID);
    std::array<CdmpDeviceInfo, 4> devices{};

    zassert_equal(service.GetNetworkDevices(devices), 0);
}

ZTEST(cdmp_service, test_device_info_follows_the_features_of_the_device) {
    const CdmpService service(DISPLAY_FEATURES, OWN_UID);

    zassert_equal(service.GetDeviceInfo().features, DISPLAY_FEATURES);

    service.GetDevice()->SetFeature(LOGGER_FEATURES, true);
    zassert_equal(service.GetDeviceInfo().features, DISPLAY_FEATURES | LOGGER_FEATURES);

    service.GetDevice()->SetFeature(DISPLAY_FEATURES, false);
    zassert_equal(service.GetDeviceInfo().features, LOGGER_FEATURES);

    service.GetDevice()->SetFeatures(0);
    zassert_equal(service.GetDeviceInfo().features, 0);
}
