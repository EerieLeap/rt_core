#pragma once

#include <memory>
#include <stdexcept>

#include "subsys/threading/i_service.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

// Shared by the calibration service and controller suites.
namespace calibration_test_support {

using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;

inline std::shared_ptr<TimeService> MakeTimeService() {
    return std::make_shared<TimeService>(std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
}

// Stands in for the processing service: running, pausable and resumable unless told otherwise,
// and counting. A throwing pause or resume stands for a reader that cannot be rebuilt.
class RecordingService : public IService {
private:
    ServiceState state_ = ServiceState::RUNNING;

public:
    bool is_pause_accepted = true;
    bool is_resume_accepted = true;
    bool is_pause_throwing = false;
    bool is_resume_throwing = false;
    int pauses = 0;
    int resumes = 0;

    bool Initialize() override {
        return true;
    }
    bool Start() override {
        state_ = ServiceState::RUNNING;
        return true;
    }
    bool Stop() override {
        state_ = ServiceState::STOPPED;
        return true;
    }

    bool Pause() override {
        pauses++;
        if(is_pause_throwing)
            throw std::runtime_error("pause failed");

        if(!is_pause_accepted)
            return false;

        state_ = ServiceState::PAUSED;
        return true;
    }

    bool Resume() override {
        resumes++;
        if(is_resume_throwing)
            throw std::runtime_error("resume failed");

        if(!is_resume_accepted)
            return false;

        state_ = ServiceState::RUNNING;
        return true;
    }

    [[nodiscard]] bool IsPausable() const noexcept override {
        return true;
    }
    [[nodiscard]] ServiceState GetState() const noexcept override {
        return state_;
    }
};

} // namespace calibration_test_support
