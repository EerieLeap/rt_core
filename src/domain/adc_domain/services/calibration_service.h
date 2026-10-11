#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <zephyr/kernel.h>

#include "subsys/adc/adc_types.h"
#include "subsys/adc/i_adc_manager.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

namespace eerie_leap::domain::adc_domain::services {

namespace threading = eerie_leap::subsys::threading;

using threading::IService;
using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::subsys::adc::AdcChannelHandle;
using eerie_leap::subsys::adc::AdcError;
using eerie_leap::subsys::adc::AdcSample;
using eerie_leap::subsys::adc::AdcSampleRequest;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::time::ITimeService;

// Samples one ADC channel on its own thread, every CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS
// with CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT conversions, while the service that normally
// owns the ADC is paused. One job at a time; the latest result is kept for the client to read.
// The service to pause is only known as an IService, so this domain never depends on its owner.
class CalibrationService {
public:
    struct Result {
        float adc_volts = 0.0F;      // ADC-pin volts, uncalibrated.
        float sensor_volts = 0.0F;   // The same through the default range, no calibration applied.
        std::chrono::system_clock::time_point timestamp;
        uint32_t sequence = 0;       // Increases with every result of the service.
        AdcError error = AdcError::NONE;
    };

private:
    static constexpr int thread_stack_size_ = 4096;
    static constexpr int thread_priority_ = 6;

    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<IAdcManager> adc_manager_;
    std::shared_ptr<IService> service_to_pause_;
    // What the running job paused, and the only service Stop() resumes.
    std::shared_ptr<IService> paused_service_;

    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<CalibrationService>> job_;

    // Serializes Start(), Stop() and SetServiceToPause(); the job never takes it, so Stop() can
    // wait for the job.
    k_mutex job_mutex_;
    // Written under job_mutex_, read without it so a status query never waits for a sample.
    std::atomic<bool> is_running_ = false;

    // A one-channel request over the members below, rebuilt by Start() while the job is idle.
    AdcChannelHandle channel_;
    uint8_t request_channel_ = 0;
    AdcSample sample_;
    AdcSampleRequest request_;

    mutable k_mutex result_mutex_;
    std::optional<Result> latest_;
    // The channel of the running or last job; latest_ always belongs to it.
    std::optional<int> job_channel_;
    uint32_t sequence_ = 0;

    void Sample();
    static WorkQueueTaskResult ProcessCalibration(CalibrationService* service);

public:
    CalibrationService(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<IAdcManager> adc_manager,
        // Paused while a job runs; null when nothing shares the ADC.
        std::shared_ptr<IService> service_to_pause);

    ~CalibrationService();

    CalibrationService(const CalibrationService&) = delete;
    CalibrationService& operator=(const CalibrationService&) = delete;

    bool Initialize();

    // Takes effect with the next Start(); a running job still resumes what it paused.
    void SetServiceToPause(std::shared_ptr<IService> service);

    // False while a job runs, for a channel the unit does not have, or when the service to pause
    // cannot be paused (another owner has it paused or stopped, or pausing throws). Never throws.
    bool Start(int channel);

    // Cancels the job and waits for it, then resumes what Start() paused, unless its owner has
    // restarted or stopped it meanwhile; a resume that fails or throws is logged and the job ends
    // all the same. False without a job. Never throws.
    bool Stop();

    [[nodiscard]] bool IsRunning() const;

    /** @return The channel of the running or last job, which the latest result belongs to. */
    [[nodiscard]] std::optional<int> GetChannel() const;

    /** @return The latest result of the running or last job, or nothing before its first sample. */
    [[nodiscard]] std::optional<Result> GetLatest() const;
};

} // namespace eerie_leap::domain::adc_domain::services
