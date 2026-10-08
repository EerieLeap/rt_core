#pragma once

#include <memory>
#include <vector>
#include <zephyr/kernel.h>

#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/i_sensor_reader.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::sensor_readers::ISensorReader;
using eerie_leap::domain::sensor_domain::processors::ReadingPipeline;

struct RateGroupEntry {
    const SensorRuntime* runtime;   // Owned by the generation the group belongs to.
    std::unique_ptr<ISensorReader> reader;
};

// Every polled sensor with the same sampling rate, read in processing order on one timer.
struct RateGroupTask {
    k_timeout_t period;
    int period_ms = 0;
    std::vector<RateGroupEntry> entries;
    std::shared_ptr<ReadingPipeline> pipeline;
};

} // namespace eerie_leap::domain::sensor_domain::services
