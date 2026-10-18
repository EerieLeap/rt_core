#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "cdmp_test_features.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/services/cdmp_canbus_service_base.h"

using eerie_leap::tests::DISPLAY_FEATURES;

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services;
using namespace eerie_leap::subsys::cdmp::utilities;

namespace {

const device* ReceiveFilterLoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// Exposes the receive registration the CDMP services share.
class ProbeService final : public CdmpCanbusServiceBase {
private:
    bool DoStart() override {
        return true;
    }
    bool DoStop() override {
        return true;
    }

public:
    using CdmpCanbusServiceBase::CdmpCanbusServiceBase;
    using CdmpCanbusServiceBase::RegisterReceiveHandler;
};

struct FrameCounter {
    k_sem received{};

    FrameCounter() {
        k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
    }

    CanFrameHandler Handler() {
        return [this](const CanFrame&) {
            k_sem_give(&received);
        };
    }

    bool Received(int timeout_ms = 200) {
        return k_sem_take(&received, K_MSEC(timeout_ms)) == 0;
    }
};

// Fills the controller's free filter slots with a filter no test frame matches, then frees them.
size_t CountFreeControllerFilters() {
    const can_filter unused{ .id = 0x0F0, .mask = CAN_STD_ID_MASK, .flags = 0 };
    std::vector<int> filter_ids;

    int filter_id = 0;
    while((filter_id = can_add_rx_filter(
               ReceiveFilterLoopbackDevice(), [](const device*, can_frame*, void*) {}, nullptr, &unused
           ))
        >= 0) {

        filter_ids.push_back(filter_id);
    }

    for(const int id : filter_ids)
        can_remove_rx_filter(ReceiveFilterLoopbackDevice(), id);

    return filter_ids.size();
}

struct ReceiveFilterFixture {
    std::shared_ptr<CdmpCanIdManager> ids = std::make_shared<CdmpCanIdManager>();
    std::shared_ptr<CdmpDevice> device = std::make_shared<CdmpDevice>(0x12345678, DISPLAY_FEATURES);
    std::shared_ptr<CanbusProxy> canbus;
    ProbeService probe{ ids, device };
    std::vector<int> handler_ids;

    ReceiveFilterFixture() {
        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(ReceiveFilterLoopbackDevice(), CanbusType::CLASSICAL_CAN, 500000, 0, CAN_MODE_LOOPBACK)
        );
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        probe.Configure(canbus);
    }

    ReceiveFilterFixture(const ReceiveFilterFixture&) = delete;
    ReceiveFilterFixture& operator=(const ReceiveFilterFixture&) = delete;
    ReceiveFilterFixture(ReceiveFilterFixture&&) = delete;
    ReceiveFilterFixture& operator=(ReceiveFilterFixture&&) = delete;

    ~ReceiveFilterFixture() {
        // Get() instead of operator->, which throws when the proxy holds no instance.
        if(Canbus* live_bus = canbus->Get(); live_bus != nullptr) {
            for(const int handler_id : handler_ids)
                live_bus->RemoveFrameReceivedHandler(handler_id);
        }
    }

    int Keep(int handler_id) {
        if(handler_id > 0)
            handler_ids.push_back(handler_id);

        return handler_id;
    }

    void Send(uint32_t id) {
        const std::array<uint8_t, 1> data{ 0x01 };
        zassert_ok((*canbus)->SendFrame(CanId::Standard(id), data, K_MSEC(100)));
    }
};

void StopReceiveFilterLoopback(void* /*fixture*/) {
    can_stop(ReceiveFilterLoopbackDevice());
}

} // namespace

ZTEST_SUITE(cdmp_receive_filter, NULL, NULL, NULL, StopReceiveFilterLoopback, NULL);

ZTEST(cdmp_receive_filter, test_receive_ids_share_one_controller_filter) {
    ReceiveFilterFixture fixture;
    const size_t free_before = CountFreeControllerFilters();
    FrameCounter management;
    FrameCounter heartbeat;

    zassert_true(
        fixture.Keep(fixture.probe.RegisterReceiveHandler(fixture.ids->GetManagementCanId(), management.Handler())) > 0
    );
    zassert_true(
        fixture.Keep(fixture.probe.RegisterReceiveHandler(fixture.ids->GetHeartbeatCanId(), heartbeat.Handler())) > 0
    );

    zassert_equal(free_before - CountFreeControllerFilters(), 1, "Both IDs share the CDMP block filter");

    fixture.Send(fixture.ids->GetManagementCanId().id);
    zassert_true(management.Received());
    zassert_false(heartbeat.Received(50), "A handler only sees the frames of its own ID");

    fixture.Send(fixture.ids->GetHeartbeatCanId().id);
    zassert_true(heartbeat.Received());
    zassert_false(management.Received(50));
}

ZTEST(cdmp_receive_filter, test_overlapping_route_makes_cdmp_use_exact_filters) {
    ReceiveFilterFixture fixture;
    FrameCounter route;
    FrameCounter management;

    // A route inside Base+0..Base+7 that registered first, as a misconfigured CAN message would.
    const uint32_t route_id = fixture.ids->GetBaseCanId() + 6;
    zassert_true(
        fixture.Keep((*fixture.canbus)->RegisterFrameReceivedHandler(CanId::Standard(route_id), route.Handler())) > 0
    );

    zassert_true(
        fixture.Keep(fixture.probe.RegisterReceiveHandler(fixture.ids->GetManagementCanId(), management.Handler())) > 0,
        "CDMP falls back to its exact filter"
    );

    fixture.Send(fixture.ids->GetManagementCanId().id);
    zassert_true(management.Received());

    fixture.Send(route_id);
    zassert_true(route.Received(), "The route keeps receiving its frames");
    zassert_false(management.Received(50));
}
