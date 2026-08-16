#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/canbus.h"

namespace canbus_test {

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::CanFrame;

inline const device* LoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// The loopback controller only echoes frames back when it is in loopback mode.
inline CanbusConfig MakeConfig(
    CanbusType type = CanbusType::CLASSICAL_CAN,
    uint32_t bitrate = 500000,
    bool is_extended_id = false,
    uint32_t data_bitrate = 0) {

    return CanbusConfig(LoopbackDevice(), type, bitrate, data_bitrate, is_extended_id, CAN_MODE_LOOPBACK);
}

inline std::unique_ptr<Canbus> MakeRunningCanbus(
    CanbusType type = CanbusType::CLASSICAL_CAN,
    uint32_t bitrate = 500000,
    bool is_extended_id = false) {

    auto canbus = std::make_unique<Canbus>(MakeConfig(type, bitrate, is_extended_id));

    zassert_true(canbus->Initialize(), "Canbus::Initialize() failed");
    zassert_true(canbus->Start(), "Canbus::Start() failed");

    return canbus;
}

// Collects frames delivered to a handler and lets a test block until one arrives.
class FrameCollector {
private:
    k_sem semaphore_{};
    k_mutex lock_{};
    std::vector<CanFrame> frames_;

public:
    FrameCollector() {
        k_sem_init(&semaphore_, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&lock_);
    }

    void Collect(const CanFrame& frame) {
        k_mutex_lock(&lock_, K_FOREVER);
        frames_.push_back(frame);
        k_mutex_unlock(&lock_);

        k_sem_give(&semaphore_);
    }

    [[nodiscard]] bool Wait(int timeout_ms = 500) {
        return k_sem_take(&semaphore_, K_MSEC(timeout_ms)) == 0;
    }

    [[nodiscard]] size_t Count() {
        k_mutex_lock(&lock_, K_FOREVER);
        size_t count = frames_.size();
        k_mutex_unlock(&lock_);

        return count;
    }

    [[nodiscard]] CanFrame At(size_t index) {
        k_mutex_lock(&lock_, K_FOREVER);
        CanFrame frame = frames_.at(index);
        k_mutex_unlock(&lock_);

        return frame;
    }
};

} // namespace canbus_test
