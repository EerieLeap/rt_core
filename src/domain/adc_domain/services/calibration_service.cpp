#include <exception>
#include <span>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/adc_domain/models/adc_range.h"
#include "subsys/threading/scoped_mutex.h"

#include "calibration_service.h"

namespace eerie_leap::domain::adc_domain::services {

using eerie_leap::domain::adc_domain::models::AdcRange;
using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_REGISTER(calibration_logger);

// The ADC would clamp a larger request silently.
static_assert(
    CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT <= CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX,
    "Calibration samples exceed CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX"
);

CalibrationService::CalibrationService(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IAdcManager> adc_manager,
    std::shared_ptr<IService> service_to_pause
) :
    time_service_(std::move(time_service)),
    adc_manager_(std::move(adc_manager)),
    service_to_pause_(std::move(service_to_pause)) {

    k_mutex_init(&job_mutex_);
    k_mutex_init(&result_mutex_);
}

// The job refers to this service, so it is stopped before anything goes.
CalibrationService::~CalibrationService() {
    Stop();
}

bool CalibrationService::Initialize() {
    if(work_queue_thread_ != nullptr)
        return true;

    auto work_queue_thread = std::make_unique<WorkQueueThread>(
        "calibration_service", THREAD_STACK_SIZE, THREAD_PRIORITY
    );
    if(!work_queue_thread->Initialize()) {
        LOG_ERR("Failed to initialize the calibration work queue.");
        return false;
    }

    work_queue_thread_ = std::move(work_queue_thread);
    job_ = work_queue_thread_->CreateTask(ProcessCalibration, this);

    return true;
}

void CalibrationService::SetServiceToPause(std::shared_ptr<IService> service) {
    ScopedMutex guard(job_mutex_);

    service_to_pause_ = std::move(service);
}

bool CalibrationService::Start(int channel) {
    ScopedMutex guard(job_mutex_);

    if(!job_.has_value() || adc_manager_ == nullptr || is_running_)
        return false;

    AdcChannelHandle channel_handle;
    try {
        channel_handle = adc_manager_->Resolve(channel);
    } catch(const std::exception& e) {
        LOG_ERR("Cannot calibrate ADC channel %d: %s", channel, e.what());
        return false;
    }

    // Called from the MCUmgr work queue, which catches nothing, so a throwing pause is a busy one.
    if(service_to_pause_ != nullptr) {
        bool is_paused = false;
        try {
            is_paused = service_to_pause_->Pause();
        } catch(const std::exception& e) {
            LOG_ERR("Cannot calibrate ADC channel %d: pausing the sampling service failed: %s", channel, e.what());
            return false;
        }

        if(!is_paused) {
            LOG_WRN("Cannot calibrate ADC channel %d: the sampling service is busy.", channel);
            return false;
        }

        paused_service_ = service_to_pause_;
    }

    channel_ = channel_handle;
    request_channel_ = channel_handle.local_channel;
    sample_ = {};
    request_ = { .channels = std::span<const uint8_t>(&request_channel_, 1),
        .results = std::span<AdcSample>(&sample_, 1),
        .samples = CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT };

    // A result of the previous job is not one of this channel.
    {
        ScopedMutex result_guard(result_mutex_);
        latest_.reset();
        job_channel_ = channel;
    }

    is_running_ = true;
    job_->Schedule();

    LOG_INF("Calibration started on ADC channel %d.", channel);

    return true;
}

bool CalibrationService::Stop() {
    ScopedMutex guard(job_mutex_);

    if(!is_running_ || !job_.has_value())
        return false;

    // The job reschedules itself, so it is cancelled until it is idle.
    while(job_->Cancel())
        k_sleep(K_MSEC(1));

    is_running_ = false;

    // A configuration change may have restarted or stopped the service meanwhile; its owner has
    // taken it back, so there is nothing to undo. The job has ended either way, also when resuming
    // throws: this runs on the MCUmgr work queue and in the destructor, which catch nothing.
    const auto paused_service = std::exchange(paused_service_, nullptr);
    if(paused_service != nullptr) {
        bool is_resumed = false;
        try {
            is_resumed = paused_service->Resume();
        } catch(const std::exception& e) {
            LOG_ERR("Calibration stopped; resuming the sampling service failed: %s", e.what());
            return true;
        }

        if(!is_resumed) {
            LOG_WRN("Calibration stopped; the sampling service was no longer paused and is left as it is.");
            return true;
        }
    }

    LOG_INF("Calibration stopped.");

    return true;
}

bool CalibrationService::IsRunning() const {
    return is_running_;
}

std::optional<int> CalibrationService::GetChannel() const {
    ScopedMutex guard(result_mutex_);

    return job_channel_;
}

std::optional<CalibrationService::Result> CalibrationService::GetLatest() const {
    ScopedMutex guard(result_mutex_);

    return latest_;
}

void CalibrationService::Sample() {
    channel_.adc->Sample(request_);

    const auto timestamp = time_service_->GetCurrentTime();

    ScopedMutex guard(result_mutex_);
    latest_ = Result{ .adc_volts = sample_.volts,
        .sensor_volts = AdcRange::Default().ToSensorVolts(sample_.volts),
        .timestamp = timestamp,
        .sequence = ++sequence_,
        .error = sample_.error };
}

WorkQueueTaskResult CalibrationService::ProcessCalibration(CalibrationService* service) {
    service->Sample();

    return { .reschedule = true, .delay = K_MSEC(CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS) };
}

} // namespace eerie_leap::domain::adc_domain::services
