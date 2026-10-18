#pragma once

#include <cstdint>
#include <memory>

#include <zephyr/ztest.h>

#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/cdmp/services/cdmp_service.h"

#include "cdmp_loopback_harness.h"
#include "cdmp_test_features.h"

using eerie_leap::tests::DISPLAY_FEATURES;

namespace cdmp_state_test {

using namespace cdmp_loopback_test;
using namespace eerie_leap::subsys::cdmp::services;

inline constexpr uint32_t OWN_UID = 0x11111111;

inline constexpr uint8_t TYPE_A = 0x10;
inline constexpr uint8_t TYPE_B = 0x11;

/** @brief A CdmpService on the loopback controller, and a peer scripted on the same bus. */
class Harness : public CdmpLoopbackHarness {
protected:
    bool StartUnit() override {
        return unit->Start();
    }
    [[nodiscard]] CdmpDeviceStatus GetUnitStatus() const override {
        return unit->GetDevice()->GetStatus();
    }
    [[nodiscard]] uint8_t GetUnitDeviceId() const override {
        return unit->GetDevice()->GetDeviceId();
    }

public:
    std::shared_ptr<CanbusProxy> canbus;
    std::shared_ptr<CdmpService> unit;

    Harness() {
        auto running_canbus = std::make_unique<Canbus>(
            CanbusConfig(LoopbackDevice(), CanbusType::CLASSICAL_CAN, 500000, 0, CAN_MODE_LOOPBACK)
        );
        zassert_true(running_canbus->Initialize(), "Canbus::Initialize() failed");
        zassert_true(running_canbus->Start(), "Canbus::Start() failed");
        canbus = std::make_shared<CanbusProxy>(std::move(running_canbus));

        AttachBus(canbus);

        unit = std::make_shared<CdmpService>(DISPLAY_FEATURES, OWN_UID);
        zassert_true(unit->Initialize());
        unit->Configure(canbus);
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() override {
        unit->Stop();
        unit.reset();
    }

    std::shared_ptr<CdmpStateService> State() const {
        return unit->GetStateService();
    }
};

} // namespace cdmp_state_test
