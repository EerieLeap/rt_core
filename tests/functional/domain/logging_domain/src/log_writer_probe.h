#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ios>
#include <memory>
#include <streambuf>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/logging_domain/loggers/i_logger.h"

namespace logging_test_support {

using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::logging_domain::loggers::ILogger;

// Hands every call to the logger it wraps, and records when the writer thread called it: the uptime
// of each tick that offered a reading. On request it holds a reading for a while, so a Stop()
// arrives in the middle of a tick, or refuses one the way a stream that failed does.
class ProbeLogger : public ILogger<SensorReading> {
private:
    std::shared_ptr<ILogger<SensorReading>> inner_;
    int64_t last_call_ticks_ = -1;

public:
    static constexpr size_t kMaxTicks = 512;
    static constexpr int32_t kHoldMs = 200;

    // Writer thread only; read once the service is stopped.
    std::vector<int64_t> tick_times;

    std::atomic<bool> refuse_next_reading = false;
    std::atomic<bool> hold_next_reading = false;
    std::atomic<bool> is_held_reading_done = false;
    std::atomic<bool> stop_found_held_reading_done = false;
    k_sem held_reading_started;

    explicit ProbeLogger(std::shared_ptr<ILogger<SensorReading>> inner) : inner_(std::move(inner)) {
        tick_times.reserve(kMaxTicks);
        k_sem_init(&held_reading_started, 0, 1);
    }

    const char* GetFileExtension() const override { return inner_->GetFileExtension(); }

    bool StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) override {
        return inner_->StartLogging(stream, start_time);
    }

    bool StopLogging() override {
        stop_found_held_reading_done = is_held_reading_done.load();

        return inner_->StopLogging();
    }

    bool LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) override {
        // Every call of one tick shares its uptime where code takes no time (native_sim).
        const int64_t now = k_uptime_ticks();
        if(now != last_call_ticks_ && tick_times.size() < kMaxTicks)
            tick_times.push_back(now);
        last_call_ticks_ = now;

        if(refuse_next_reading.exchange(false))
            throw std::ios_base::failure("The probe refuses the record.");

        if(!hold_next_reading.exchange(false))
            return inner_->LogReading(time, reading);

        k_sem_give(&held_reading_started);
        k_msleep(kHoldMs);

        const bool is_logged = inner_->LogReading(time, reading);
        is_held_reading_done = true;

        return is_logged;
    }

    [[nodiscard]] uint64_t GetBytesWritten() const override { return inner_->GetBytesWritten(); }
};

} // namespace logging_test_support
