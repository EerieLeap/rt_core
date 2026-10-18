// The log writer's deadlines against the clock. native_sim only, where code takes no simulated
// time: every tick lands on its deadline, so periods and phases come out exact.
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor_limits.h"

#include "log_writer_probe.h"
#include "logging_test_support.h"

#ifdef CONFIG_BOARD_NATIVE_SIM

using namespace logging_test_support;
using eerie_leap::domain::sensor_domain::models::SensorLimits;

namespace {

int64_t MsToTicks(int64_t ms) {
    return static_cast<int64_t>(k_ms_to_ticks_ceil64(static_cast<uint64_t>(ms)));
}

// The rig over MakeLoggedSensors() with a probe around the logger, and a held value in the frame,
// so that every tick offers the logger a reading and the probe sees every tick.
std::shared_ptr<ProbeLogger> AttachProbe(LogWriterRig& rig, const Sensor& value_sensor) {
    auto probe = std::make_shared<ProbeLogger>(rig.logger_factory());
    rig.log_writer_service->SetLoggerFactory([probe] {
        return probe;
    });

    PublishValue(rig, value_sensor, 1, rig.time_service->GetCurrentTime());

    return probe;
}

} // namespace

ZTEST_SUITE(log_writer_timing, NULL, NULL, ResetAfterTest, NULL, NULL);

// Before Phase 4 the tick re-armed a relative interval when it ended: the tick's own duration and
// the kernel's rounding of a relative timeout up to the next tick edge added to every period.
// Absolute deadlines add neither.
ZTEST(log_writer_timing, test_TwentyMillisecondTicksKeepTheirPhase) {
    constexpr int64_t interval_ms = 20;
    constexpr size_t ticks = 100;

    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors, false);
    StoreLoggingConfiguration(rig, interval_ms);
    const auto probe = AttachProbe(rig, *sensors[0]);

    const int64_t interval_ticks = MsToTicks(interval_ms);
    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    k_sleep(K_TIMEOUT_ABS_TICKS(start_ticks + static_cast<int64_t>(ticks) * interval_ticks + interval_ticks / 2));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto& times = probe->tick_times;
    zassert_true(times.size() >= ticks, "%zu ticks in %zu intervals", times.size(), ticks);
    zassert_true(
        std::abs(times[0] - (start_ticks + interval_ticks)) <= 1,
        "the first tick is one interval after Start(), not %lld ticks", static_cast<long long>(times[0] - start_ticks)
    );

    // No drift: every tick on the phase the first one set, within one kernel tick.
    for(size_t i = 1; i < ticks; i++) {
        const int64_t deviation = times[i] - times[0] - static_cast<int64_t>(i) * interval_ticks;
        zassert_true(
            std::abs(deviation) <= 1, "tick %zu is %lld kernel ticks off the phase", i,
            static_cast<long long>(deviation)
        );
    }

    const int64_t total_ticks = times[ticks - 1] - times[0];
    const int64_t expected_ticks = static_cast<int64_t>(ticks - 1) * interval_ticks;
    zassert_true(
        std::abs(total_ticks - expected_ticks) <= 1, "mean period %lld/%zu kernel ticks, %lld expected per tick",
        static_cast<long long>(total_ticks), ticks - 1, static_cast<long long>(interval_ticks)
    );

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.overruns, 0, "a tick that takes no time overruns nothing (got %u)", stats.overruns);
}

// A drain that empties a full raw queue may have left frames behind: the next tick comes a quarter
// of an interval later, and the ones after it an interval apart again.
ZTEST(log_writer_timing, test_AFullDrainBringsTheNextTickForward) {
    constexpr int64_t interval_ms = 200;

    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors, false);
    StoreLoggingConfiguration(rig, interval_ms);
    const auto probe = AttachProbe(rig, *sensors[0]);

    const int64_t start_ticks = k_uptime_ticks();
    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    for(size_t i = 0; i < SensorLimits::kRawCanQueueSize; i++)
        PublishFrame(rig, *sensors[1], static_cast<uint8_t>(i), rig.time_service->GetCurrentTime());

    k_sleep(K_TIMEOUT_ABS_TICKS(start_ticks + MsToTicks(500)));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.frames, SensorLimits::kRawCanQueueSize, "every queued frame is logged (got %u)", stats.frames);
    zassert_equal(stats.frames_dropped, 0, "a queue filled to its size drops nothing (got %u)", stats.frames_dropped);

    const auto& times = probe->tick_times;
    zassert_equal(times.size(), 3, "ticks at 200, 250 and 450 ms (got %zu)", times.size());
    zassert_true(
        std::abs(times[0] - start_ticks - MsToTicks(interval_ms)) <= 1, "the first tick is one interval after Start()"
    );
    zassert_true(
        std::abs(times[1] - times[0] - MsToTicks(interval_ms / 4)) <= 1,
        "the tick after a full drain comes a quarter interval later, not %lld kernel ticks",
        static_cast<long long>(times[1] - times[0])
    );
    zassert_true(
        std::abs(times[2] - times[1] - MsToTicks(interval_ms)) <= 1,
        "the next one an interval later, not %lld kernel ticks", static_cast<long long>(times[2] - times[1])
    );
}

#endif // CONFIG_BOARD_NATIVE_SIM
