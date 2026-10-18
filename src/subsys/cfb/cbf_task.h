#pragma once

#include <functional>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace eerie_leap::subsys::cfb {

struct CfbTask {
    const device* display_dev = nullptr;
    atomic_t is_animation_running_{ ATOMIC_INIT(0) };
    std::function<void()> animation_handler;
    uint32_t frame_rate = 0;

    CfbTask() = default;
};

} // namespace eerie_leap::subsys::cfb
