#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/i_sensor_reader.h"

#include "adc_sample_set.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::sensor_readers::ISensorReader;

struct RateGroupEntry {
    const SensorRuntime* runtime; // Owned by the generation the group belongs to.
    std::unique_ptr<ISensorReader> reader;
};

// Written by the processing thread only; atomic so another thread can read them while the group
// runs. A reader sees each field whole, not the fields of one tick together.
struct RateGroupStats {
    atomic_t ticks = ATOMIC_INIT(0);
    atomic_t overruns = ATOMIC_INIT(0); // Deadlines skipped because a tick ran past them.
    atomic_t last_us = ATOMIC_INIT(0);  // Duration of the latest tick.
    atomic_t max_us = ATOMIC_INIT(0);   // Longest tick since the group was built.

    void RecordTick(uint32_t tick_us) {
        atomic_set(&last_us, static_cast<atomic_val_t>(tick_us));
        if(tick_us > static_cast<uint32_t>(atomic_get(&max_us)))
            atomic_set(&max_us, static_cast<atomic_val_t>(tick_us));

        atomic_inc(&ticks);
    }

    void RecordOverruns(uint32_t missed) {
        atomic_add(&overruns, static_cast<atomic_val_t>(missed));
    }
};

struct RateGroupStatsSnapshot {
    int period_ms = 0;
    uint64_t period_us = 0; // What the group runs at: period_ms rounded up to whole system ticks.
    size_t sensors = 0;
    size_t adc_channels = 0;
    uint32_t ticks = 0;
    uint32_t overruns = 0;
    uint32_t last_us = 0;
    uint32_t max_us = 0;
};

#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    // Tick starts are taken from the 64-bit counter where there is one, so a slow group does not wrap
    // it: the 32-bit counter wraps after 17.9 s at 240 MHz and 8.9 s at 480 MHz.
    #ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
using RateGroupCycles = uint64_t;
    #else
using RateGroupCycles = uint32_t;
    #endif

// The delivered period, tick start to tick start, over one stats log interval. Processing thread only.
struct RateGroupPeriodWindow {
    RateGroupCycles last_start_cycles = 0;
    bool has_last_start = false;
    uint32_t periods = 0;
    uint64_t sum_cycles = 0;
    RateGroupCycles min_cycles = std::numeric_limits<RateGroupCycles>::max();
    RateGroupCycles max_cycles = 0;

    // Unsigned subtraction, so the 32-bit counter's wrap between two starts is harmless.
    void RecordStart(RateGroupCycles start_cycles) {
        if(has_last_start) {
            const RateGroupCycles period_cycles = start_cycles - last_start_cycles;
            periods++;
            sum_cycles += period_cycles;
            min_cycles = std::min(min_cycles, period_cycles);
            max_cycles = std::max(max_cycles, period_cycles);
        }

        last_start_cycles = start_cycles;
        has_last_start = true;
    }

    // The interval after a log line goes on from the last tick start, so no period is lost.
    void StartInterval() {
        periods = 0;
        sum_cycles = 0;
        min_cycles = std::numeric_limits<RateGroupCycles>::max();
        max_cycles = 0;
    }

    // A stop or a pause is not a period.
    void Restart() {
        StartInterval();
        has_last_start = false;
    }
};
#endif

// Every polled sensor with the same sampling rate, read in processing order at one deadline.
// The sample set is declared before the entries, so the readers pointing into it go first.
struct RateGroup {
    int period_ms = 0;
    k_ticks_t period_ticks = 0;
    int64_t next_deadline = 0; // Absolute, in system ticks.
    AdcSampleSet adc_samples;
    std::vector<RateGroupEntry> entries; // Processing order.
    RateGroupStats stats;
#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS
    RateGroupPeriodWindow period_window;
#endif

    [[nodiscard]] RateGroupStatsSnapshot GetStatsSnapshot() const {
        return { .period_ms = period_ms,
            .period_us = k_ticks_to_us_floor64(static_cast<uint64_t>(period_ticks)),
            .sensors = entries.size(),
            .adc_channels = adc_samples.GetChannelCount(),
            .ticks = static_cast<uint32_t>(atomic_get(&stats.ticks)),
            .overruns = static_cast<uint32_t>(atomic_get(&stats.overruns)),
            .last_us = static_cast<uint32_t>(atomic_get(&stats.last_us)),
            .max_us = static_cast<uint32_t>(atomic_get(&stats.max_us)) };
    }
};

} // namespace eerie_leap::domain::sensor_domain::services
