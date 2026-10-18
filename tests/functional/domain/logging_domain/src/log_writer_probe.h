#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <ios>
#include <memory>
#include <streambuf>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/logging_domain/loggers/i_logger.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "subsys/threading/scoped_mutex.h"

namespace logging_test_support {

using eerie_leap::domain::logging_domain::loggers::ILogger;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::subsys::threading::ScopedMutex;

// Hands every call to the logger it wraps, and records when the writer thread called it: the uptime
// of each tick that offered a reading, and the thread that finalized the file. On request it holds
// a reading for a while, so a Stop() arrives in the middle of a tick, refuses one the way a stream
// that failed does, or throws from StopLogging() so a stop fails.
class ProbeLogger : public ILogger<SensorReading> {
private:
    std::shared_ptr<ILogger<SensorReading>> inner_;
    int64_t last_call_ticks_ = -1;

public:
    static constexpr size_t MAX_TICKS = 512;
    static constexpr int32_t HOLD_MS = 200;

    // Writer thread only; read once the service is stopped.
    std::vector<int64_t> tick_times;

    std::atomic<bool> refuse_next_reading = false;
    std::atomic<bool> hold_next_reading = false;
    // The next this many StopLogging() calls throw, which a stop cannot get past.
    std::atomic<int> stop_throws = 0;
    std::atomic<k_tid_t> stop_thread = nullptr;
    std::atomic<bool> is_held_reading_done = false;
    std::atomic<bool> stop_found_held_reading_done = false;
    k_sem held_reading_started{};

    explicit ProbeLogger(std::shared_ptr<ILogger<SensorReading>> inner) :
        inner_(std::move(inner)) {
        tick_times.reserve(MAX_TICKS);
        k_sem_init(&held_reading_started, 0, 1);
    }

    const char* GetFileExtension() const override {
        return inner_->GetFileExtension();
    }

    bool StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) override {
        return inner_->StartLogging(stream, start_time);
    }

    bool StopLogging() override {
        if(stop_throws > 0) {
            stop_throws--;
            throw std::ios_base::failure("The probe refuses to finalize.");
        }

        stop_thread = k_current_get();
        stop_found_held_reading_done = is_held_reading_done.load();

        return inner_->StopLogging();
    }

    bool LogReading(const std::chrono::system_clock::time_point& time, const SensorReading& reading) override {
        // Every call of one tick shares its uptime where code takes no time (native_sim).
        const int64_t now = k_uptime_ticks();
        if(now != last_call_ticks_ && tick_times.size() < MAX_TICKS)
            tick_times.push_back(now);
        last_call_ticks_ = now;

        if(refuse_next_reading.exchange(false))
            throw std::ios_base::failure("The probe refuses the record.");

        if(!hold_next_reading.exchange(false))
            return inner_->LogReading(time, reading);

        k_sem_give(&held_reading_started);
        k_msleep(HOLD_MS);

        const bool is_logged = inner_->LogReading(time, reading);
        is_held_reading_done = true;

        return is_logged;
    }

    [[nodiscard]] uint64_t GetBytesWritten() const override {
        return inner_->GetBytesWritten();
    }
};

// A logger factory that wraps every logger another one builds in a new ProbeLogger, so a case can
// reach the probe of the session that runs now across restarts. Shared, so the factory the writer
// holds keeps it, not the test's stack.
class ProbeLoggerFactory : public std::enable_shared_from_this<ProbeLoggerFactory> {
public:
    using LoggerFactory = std::function<std::shared_ptr<ILogger<SensorReading>>()>;

private:
    LoggerFactory inner_;

    mutable k_mutex lock_{};
    std::shared_ptr<ProbeLogger> current_;
    int count_ = 0;

    explicit ProbeLoggerFactory(LoggerFactory inner) :
        inner_(std::move(inner)) {
        k_mutex_init(&lock_);
    }

    // On the thread that starts the writer.
    std::shared_ptr<ILogger<SensorReading>> Create() {
        auto inner = inner_();
        if(inner == nullptr)
            return nullptr;

        auto probe = std::make_shared<ProbeLogger>(std::move(inner));

        ScopedMutex guard(lock_);
        current_ = probe;
        count_++;

        return probe;
    }

public:
    static std::shared_ptr<ProbeLoggerFactory> Make(LoggerFactory inner) {
        return std::shared_ptr<ProbeLoggerFactory>(new ProbeLoggerFactory(std::move(inner)));
    }

    LoggerFactory AsFactory() {
        return [self = shared_from_this()] {
            return self->Create();
        };
    }

    // The probe of the latest start; null before the first.
    std::shared_ptr<ProbeLogger> Current() const {
        ScopedMutex guard(lock_);

        return current_;
    }

    int GetCount() const {
        ScopedMutex guard(lock_);

        return count_;
    }
};

} // namespace logging_test_support
