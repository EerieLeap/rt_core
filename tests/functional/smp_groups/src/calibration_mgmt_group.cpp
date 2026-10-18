#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include "domain/adc_domain/models/adc_range.h"
#include "domain/adc_domain/services/calibration_service.h"
#include "domain/adc_domain/smp/calibration_mgmt_group.h"
#include "subsys/adc/adc_manager.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/threading/i_service.h"
#include "subsys/time/i_time_service.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::domain::adc_domain::models::AdcRange;
using eerie_leap::domain::adc_domain::services::CalibrationService;
using eerie_leap::domain::adc_domain::smp::CalibrationMgmtGroup;
using eerie_leap::subsys::adc::AdcCapabilities;
using eerie_leap::subsys::adc::AdcError;
using eerie_leap::subsys::adc::AdcManager;
using eerie_leap::subsys::adc::AdcSampleRequest;
using eerie_leap::subsys::adc::AdcSimulator;
using eerie_leap::subsys::adc::AdcStats;
using eerie_leap::subsys::adc::IAdc;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::time::ITimeService;

using Command = CalibrationMgmtGroup::Command;
using Error = CalibrationMgmtGroup::Error;

namespace {

// Past 2^32 ms, so the timestamp only fits the 64-bit encoding.
constexpr uint64_t NOW_MS = 1'790'000'000'123ULL;

uint8_t Id(Command command) {
    return std::to_underlying(command);
}

void AssertError(const Response& response, Error error) {
    zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::CALIBRATION));
    zassert_equal(
        response.error.value_or(0), std::to_underlying(error), "Expected error %u, got %u", std::to_underlying(error),
        response.error.value_or(0)
    );
}

// A clock that stands still, so a result's timestamp is known exactly.
class FixedTimeService : public ITimeService {
public:
    std::chrono::system_clock::time_point GetCurrentTime() override {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(NOW_MS));
    }

    std::chrono::system_clock::time_point GetTimeSinceBoot() override {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(k_uptime_get()));
    }
};

// Stands in for the processing service: pausable unless told otherwise, and counting.
class RecordingService : public IService {
private:
    ServiceState state_ = ServiceState::RUNNING;

public:
    bool is_pause_accepted = true;
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
        if(!is_pause_accepted)
            return false;

        state_ = ServiceState::PAUSED;
        return true;
    }

    bool Resume() override {
        resumes++;
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

// One channel whose conversions wait until the gate opens, so a job can be caught before its
// first result. Open, it reads a constant.
class GatedAdc : public IAdc {
private:
    k_sem gate_;

public:
    static constexpr float VOLTS = 1.25F;

    GatedAdc() {
        k_sem_init(&gate_, 0, 1);
    }

    void Open() {
        k_sem_give(&gate_);
    }

    bool Initialize() override {
        return true;
    }

    void Sample(const AdcSampleRequest& request) noexcept override {
        k_sem_take(&gate_, K_FOREVER);
        k_sem_give(&gate_);

        for(auto& result : request.results)
            result = { .volts = VOLTS };
    }

    [[nodiscard]] int GetChannelCount() const override {
        return 1;
    }
    [[nodiscard]] AdcCapabilities GetCapabilities() const override {
        return {};
    }
    [[nodiscard]] AdcStats GetStats() const override {
        return {};
    }
};

// The group over a calibration service on the ADC simulator (eight channels) unless given another
// ADC, as AdcController wires it on a unit without ADC hardware.
class Harness {
public:
    std::shared_ptr<AdcManager> adc_manager;
    std::shared_ptr<RecordingService> paused_service = std::make_shared<RecordingService>();
    std::shared_ptr<CalibrationService> calibration_service;
    std::unique_ptr<CalibrationMgmtGroup> group;
    SmpTestClient client;

    explicit Harness(std::shared_ptr<IAdc> adc = std::make_shared<AdcSimulator>()) {
        adc_manager = std::make_shared<AdcManager>(std::vector<std::shared_ptr<IAdc>>{ std::move(adc) });
        zassert_true(adc_manager->Initialize());

        calibration_service = std::make_shared<CalibrationService>(
            std::make_shared<FixedTimeService>(), adc_manager, paused_service
        );
        zassert_true(calibration_service->Initialize());

        group = std::make_unique<CalibrationMgmtGroup>(calibration_service, adc_manager->GetChannelCount());
        group->Register();
    }

    // The group is unregistered before the service it dispatches to goes.
    ~Harness() {
        group.reset();
        calibration_service.reset();
    }

    Response Start(uint32_t channel) {
        return client.Write(SmpGroupId::CALIBRATION, Id(Command::START), CborMap().Put("channel", channel).Build());
    }

    Response Stop() {
        return client.Write(SmpGroupId::CALIBRATION, Id(Command::STOP), CborMap().Build());
    }

    Response Read() {
        return client.Read(SmpGroupId::CALIBRATION, Id(Command::READ));
    }

    // Reads until the job has a result newer than @p after_sequence.
    Response ReadAfter(uint32_t after_sequence) {
        const int64_t end = k_uptime_get() + 10 * CONFIG_EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS;
        auto response = Read();
        while(response.IsOk() && response.Value("seq") <= after_sequence && k_uptime_get() < end) {
            k_msleep(5);
            response = Read();
        }

        return response;
    }
};

} // namespace

ZTEST_SUITE(smp_calibration_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_calibration_group, test_start_read_stop_round_trip) {
    Harness harness;

    zassert_true(harness.Start(2).IsOk());
    zassert_true(harness.calibration_service->IsRunning());
    zassert_equal(harness.paused_service->pauses, 1);
    zassert_true(harness.paused_service->IsPaused());

    const auto first = harness.ReadAfter(0);
    zassert_true(first.IsOk());
    zassert_true(first.Value("seq") >= 1, "The job produced a result");
    zassert_equal(first.Value("channel"), 2);
    zassert_equal(first.Value("adc_err"), std::to_underlying(AdcError::NONE));
    zassert_true(first.Flag("running"));
    zassert_equal(first.WideValue("ts"), NOW_MS);

    const float adc_volts = first.Float("adc_v");
    zassert_true(adc_volts >= 0.0F && adc_volts <= 3.3F, "The simulator samples 0 to 3.3 V");
    zassert_within(first.Float("sensor_v"), AdcRange::Default().ToSensorVolts(adc_volts), 1e-6F);

    const auto second = harness.ReadAfter(first.Value("seq"));
    zassert_true(second.Value("seq") > first.Value("seq"), "The job keeps sampling");

    // One job at a time, and the second START pauses nothing.
    AssertError(harness.Start(2), Error::BUSY);
    AssertError(harness.Start(3), Error::BUSY);
    zassert_equal(harness.paused_service->pauses, 1);

    zassert_true(harness.Stop().IsOk());
    zassert_false(harness.calibration_service->IsRunning());
    zassert_equal(harness.paused_service->resumes, 1);
    zassert_true(harness.paused_service->IsRunning());

    // The last result stays readable.
    const auto last = harness.Read();
    zassert_true(last.IsOk());
    zassert_false(last.Flag("running"));
    zassert_equal(last.Value("channel"), 2);
    zassert_equal(last.Value("seq"), harness.calibration_service->GetLatest()->sequence);
    zassert_true(last.Value("seq") >= second.Value("seq"));
    zassert_equal(last.Value("adc_err"), std::to_underlying(AdcError::NONE));
    zassert_equal(last.WideValue("ts"), NOW_MS);
    zassert_within(last.Float("sensor_v"), AdcRange::Default().ToSensorVolts(last.Float("adc_v")), 1e-6F);

    AssertError(harness.Stop(), Error::NOT_RUNNING);
    zassert_equal(harness.paused_service->resumes, 1, "Resumed once, by the STOP that ended the job");
}

ZTEST(smp_calibration_group, test_a_new_job_starts_after_stop) {
    Harness harness;

    zassert_true(harness.Start(1).IsOk());
    const auto first_job = harness.ReadAfter(0);
    zassert_equal(first_job.Value("channel"), 1);
    zassert_true(harness.Stop().IsOk());
    const uint32_t first_job_last = harness.calibration_service->GetLatest()->sequence;

    zassert_true(harness.Start(5).IsOk());
    zassert_equal(harness.paused_service->pauses, 2);

    // A result of the previous job is never reported for the new channel: right after START the
    // job has either no result yet or one of its own.
    const auto started = harness.Read();
    zassert_true(started.IsOk());
    zassert_equal(started.Value("channel"), 5);
    zassert_true(
        started.Value("seq") == 0 || started.Value("seq") > first_job_last, "seq %u is the first job's (last %u)",
        started.Value("seq"), first_job_last
    );
    if(started.Value("seq") == 0)
        zassert_equal(started.Float("adc_v"), 0.0F);

    const auto second_job = harness.ReadAfter(first_job_last);
    zassert_true(second_job.Flag("running"));
    zassert_equal(second_job.Value("channel"), 5);
    zassert_true(second_job.Value("seq") > first_job.Value("seq"));

    zassert_true(harness.Stop().IsOk());
    zassert_equal(harness.paused_service->resumes, 2);
}

ZTEST(smp_calibration_group, test_read_before_the_first_result) {
    auto adc = std::make_shared<GatedAdc>();
    Harness harness(adc);

    zassert_true(harness.Start(0).IsOk());

    // The job is inside its first conversion: running, with nothing sampled.
    const auto pending = harness.Read();
    zassert_true(pending.IsOk());
    zassert_true(pending.Flag("running"));
    zassert_equal(pending.Value("channel"), 0);
    zassert_equal(pending.Value("seq"), 0);
    zassert_equal(pending.Float("adc_v"), 0.0F);
    zassert_equal(pending.Value("adc_err"), std::to_underlying(AdcError::NONE));

    adc->Open();

    const auto sampled = harness.ReadAfter(0);
    zassert_true(sampled.Value("seq") >= 1);
    zassert_within(sampled.Float("adc_v"), GatedAdc::VOLTS, 1e-6F);

    zassert_true(harness.Stop().IsOk());
    zassert_equal(harness.paused_service->resumes, 1);
}

ZTEST(smp_calibration_group, test_start_rejects_a_channel_the_unit_does_not_have) {
    Harness harness;
    const auto channel_count = static_cast<uint32_t>(harness.adc_manager->GetChannelCount());

    AssertError(harness.Start(99), Error::BAD_CHANNEL);
    AssertError(harness.Start(channel_count), Error::BAD_CHANNEL);
    zassert_false(harness.calibration_service->IsRunning());
    zassert_equal(harness.paused_service->pauses, 0, "Nothing is paused for a bad channel");

    // The last channel is a good one.
    zassert_true(harness.Start(channel_count - 1).IsOk());
    zassert_true(harness.Stop().IsOk());
}

ZTEST(smp_calibration_group, test_start_is_busy_while_the_service_refuses_to_pause) {
    Harness harness;
    harness.paused_service->is_pause_accepted = false;

    AssertError(harness.Start(1), Error::BUSY);
    zassert_equal(harness.paused_service->pauses, 1);
    zassert_false(harness.calibration_service->IsRunning());
    AssertError(harness.Read(), Error::NOT_RUNNING);
    AssertError(harness.Stop(), Error::NOT_RUNNING);
    zassert_equal(harness.paused_service->resumes, 0, "Nothing it did not pause is resumed");

    harness.paused_service->is_pause_accepted = true;
    zassert_true(harness.Start(1).IsOk());
    zassert_true(harness.Stop().IsOk());
    zassert_equal(harness.paused_service->resumes, 1);
}

ZTEST(smp_calibration_group, test_read_and_stop_without_a_job_are_rejected) {
    Harness harness;

    AssertError(harness.Read(), Error::NOT_RUNNING);
    AssertError(harness.Stop(), Error::NOT_RUNNING);
    zassert_equal(harness.paused_service->pauses, 0);
    zassert_equal(harness.paused_service->resumes, 0);
}

ZTEST(smp_calibration_group, test_start_without_a_channel_is_invalid) {
    Harness harness;

    auto response = harness.client.Write(SmpGroupId::CALIBRATION, Id(Command::START), CborMap().Build());
    zassert_false(response.IsOk());
    zassert_equal(response.Value("rc"), MGMT_ERR_EINVAL);
    zassert_false(harness.calibration_service->IsRunning());
    zassert_equal(harness.paused_service->pauses, 0);
}
