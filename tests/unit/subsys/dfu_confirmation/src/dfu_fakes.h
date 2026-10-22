#pragma once

#include <cstdint>

#include "subsys/dfu/i_boot_control.h"
#include "subsys/dfu/i_probe.h"
#include "subsys/dfu/i_watchdog.h"

namespace dfu_fakes {

using eerie_leap::subsys::dfu::IBootControl;
using eerie_leap::subsys::dfu::IProbe;
using eerie_leap::subsys::dfu::IWatchdog;

class FakeBootControl final : public IBootControl {
public:
    bool confirmed = false;
    int confirm_result = 0;
    mutable int is_confirmed_calls = 0;
    int confirm_calls = 0;

    [[nodiscard]] bool IsConfirmed() const override {
        ++is_confirmed_calls;
        return confirmed;
    }

    [[nodiscard]] int Confirm() override {
        ++confirm_calls;
        if(confirm_result == 0)
            confirmed = true;

        return confirm_result;
    }
};

class FakeWatchdog final : public IWatchdog {
public:
    int install_result = 0;
    int start_result = 0;
    int feed_result = 0;
    int disable_result = 0;

    int install_calls = 0;
    int start_calls = 0;
    int feed_calls = 0;
    int disable_calls = 0;
    uint32_t installed_timeout_ms = 0;
    /// Started and not disabled: the unit would reset without a feed.
    bool is_running = false;

    [[nodiscard]] int Install(uint32_t timeout_ms) override {
        ++install_calls;
        installed_timeout_ms = timeout_ms;
        return install_result;
    }

    [[nodiscard]] int Start() override {
        ++start_calls;
        if(start_result == 0)
            is_running = true;

        return start_result;
    }

    [[nodiscard]] int Feed() override {
        ++feed_calls;
        return feed_result;
    }

    [[nodiscard]] int Disable() override {
        ++disable_calls;
        if(disable_result == 0)
            is_running = false;

        return disable_result;
    }
};

/// Runs only when the test says so: Arm() leaves it pending and Run() is the work queue getting to it.
class FakeProbe final : public IProbe {
private:
    const char* name_;

public:
    bool ran = false;
    bool is_pending = false;
    /// A stuck probe never runs, like a work queue that is blocked.
    bool is_stuck = false;
    int arm_calls = 0;

    explicit FakeProbe(const char* name) :
        name_(name) {}

    void Arm() override {
        ++arm_calls;
        ran = false;
        is_pending = true;
    }

    [[nodiscard]] bool Check() const override {
        return ran;
    }

    [[nodiscard]] const char* GetName() const override {
        return name_;
    }

    void Run() {
        if(!is_pending || is_stuck)
            return;

        is_pending = false;
        ran = true;
    }
};

} // namespace dfu_fakes
