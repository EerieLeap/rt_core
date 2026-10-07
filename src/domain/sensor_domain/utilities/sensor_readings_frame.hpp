#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include <zephyr/kernel.h>

#include "utilities/string/string_helpers.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/canbus/can_frame.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::ReadingSource;

// The latest reading of every sensor, shared between the processing pipeline and its consumers.
//
// Entries are created the first time a sensor is seen (Reserve() sizes the tables up front) and
// updated in place afterwards, so a steady-state commit or snapshot does not allocate. Consumers
// copy into buffers they own; nothing escapes the lock by reference except the value slots of
// GetReadingValuePtr(), which are never freed.
class SensorReadingsFrame {
private:
    struct ProcessedEntry {
        SensorReading reading;
        bool is_updated = false;   // Changed since the last TakeProcessedReadings().
    };

    std::unordered_map<uint32_t, SensorReading> readings_;             // Latest reading, any status.
    std::unordered_map<uint32_t, ProcessedEntry> processed_readings_;  // Latest reading with status PROCESSED.
    std::unordered_map<uint32_t, float> reading_values_;               // Latest processed value, NaN for none.
    std::unordered_map<uint32_t, CanFrame> can_frames_;                // CANBUS_RAW sensors only.

    // Methods may allocate while holding it (first sight of a sensor), so the guard must release on a throw.
    mutable k_mutex lock_;

    static constexpr float kNoValue = std::numeric_limits<float>::quiet_NaN();

    static uint32_t GetSensorIdHash(std::string_view sensor_id) {
        return StringHelpers::GetHash(sensor_id);
    }

    static std::optional<float> ToOptional(float value) {
        return std::isnan(value) ? std::nullopt : std::optional<float>(value);
    }

public:
    SensorReadingsFrame() {
        k_mutex_init(&lock_);
    }

    SensorReadingsFrame(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame(SensorReadingsFrame&&) = delete;
    SensorReadingsFrame& operator=(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame& operator=(SensorReadingsFrame&&) = delete;

    /** @brief Sizes the tables for @p sensor_count sensors so later inserts do not rehash. */
    void Reserve(size_t sensor_count) {
        ScopedMutex guard(lock_);

        readings_.reserve(sensor_count);
        processed_readings_.reserve(sensor_count);
        reading_values_.reserve(sensor_count);
    }

    /**
     * @brief Stores @p reading as the sensor's latest state. A reading without a source is ignored.
     *
     * A PROCESSED reading also becomes the sensor's latest processed reading and, when it has a
     * value, its latest value. A CAN frame referenced by the reading is copied into the frame store.
     */
    void AddOrUpdateReading(const SensorReading& reading) {
        if(reading.source == ReadingSource::NONE)
            return;

        const uint32_t sensor_id_hash = reading.sensor_id_hash;

        ScopedMutex guard(lock_);

        if(reading.can_frame != nullptr)
            can_frames_.insert_or_assign(sensor_id_hash, *reading.can_frame);

        // Stored copies never point at a caller's frame.
        SensorReading stored = reading;
        stored.can_frame = nullptr;

        readings_.insert_or_assign(sensor_id_hash, stored);

        if(stored.status != ReadingStatus::PROCESSED)
            return;

        auto& entry = processed_readings_[sensor_id_hash];
        entry.reading = stored;
        entry.is_updated = true;

        if(stored.value.has_value())
            reading_values_[sensor_id_hash] = stored.value.value();
    }

    std::optional<SensorReading> TryGetReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        const auto it = readings_.find(sensor_id_hash);

        return it != readings_.end() ? std::optional<SensorReading>(it->second) : std::nullopt;
    }

    std::optional<SensorReading> TryGetReading(std::string_view sensor_id) const {
        return TryGetReading(GetSensorIdHash(sensor_id));
    }

    std::optional<float> TryGetReadingValue(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        const auto it = reading_values_.find(sensor_id_hash);

        return it != reading_values_.end() ? ToOptional(it->second) : std::nullopt;
    }

    std::optional<float> TryGetReadingValue(std::string_view sensor_id) const {
        return TryGetReadingValue(GetSensorIdHash(sensor_id));
    }

    /**
     * @brief Copies the latest processed values of @p sensor_id_hashes under a single lock.
     * @param values One entry per hash, empty for a sensor without a processed value yet.
     */
    void GetReadingValues(std::span<const uint32_t> sensor_id_hashes, std::span<std::optional<float>> values) const {
        const size_t count = std::min(sensor_id_hashes.size(), values.size());

        ScopedMutex guard(lock_);

        for(size_t i = 0; i < count; i++) {
            const auto it = reading_values_.find(sensor_id_hashes[i]);
            values[i] = it != reading_values_.end() ? ToOptional(it->second) : std::nullopt;
        }
    }

    /**
     * @brief Address of the sensor's latest processed value, for evaluators that read it directly.
     *
     * The slot is created on first request and holds NaN until the sensor has a processed value, so
     * the pointer is never null and survives ClearReadings().
     */
    float* GetReadingValuePtr(std::string_view sensor_id) {
        ScopedMutex guard(lock_);

        return &reading_values_.try_emplace(GetSensorIdHash(sensor_id), kNoValue).first->second;
    }

    /** @brief Copies the latest CAN frame stored for a CANBUS_RAW sensor. */
    bool TryGetCanFrame(const uint32_t sensor_id_hash, CanFrame& can_frame) const {
        ScopedMutex guard(lock_);

        const auto it = can_frames_.find(sensor_id_hash);
        if(it == can_frames_.end())
            return false;

        can_frame = it->second;

        return true;
    }

    [[nodiscard]] size_t GetProcessedReadingCount() const {
        ScopedMutex guard(lock_);

        return processed_readings_.size();
    }

    /**
     * @brief Copies the latest processed reading of every sensor into @p readings.
     * @return The number copied, at most readings.size().
     */
    size_t SnapshotProcessedReadings(std::span<SensorReading> readings) const {
        ScopedMutex guard(lock_);

        size_t count = 0;
        for(const auto& [_, entry] : processed_readings_) {
            if(count == readings.size())
                break;

            readings[count++] = entry.reading;
        }

        return count;
    }

    /**
     * @brief Copies the readings processed since they were last taken into @p readings and marks
     *        those as taken, in one step, so no update is lost between the copy and the reset.
     *
     * Readings that do not fit stay pending for the next call.
     * @return The number copied, at most readings.size().
     */
    size_t TakeProcessedReadings(std::span<SensorReading> readings) {
        ScopedMutex guard(lock_);

        size_t count = 0;
        for(auto& [_, entry] : processed_readings_) {
            if(count == readings.size())
                break;

            if(!entry.is_updated)
                continue;

            readings[count++] = entry.reading;
            entry.is_updated = false;
        }

        return count;
    }

    bool HasReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        return readings_.contains(sensor_id_hash);
    }

    void ClearProcessedReadings() {
        ScopedMutex guard(lock_);

        processed_readings_.clear();
    }

    /** @brief Drops every reading. Value slots are kept, see GetReadingValuePtr(). */
    void ClearReadings() {
        ScopedMutex guard(lock_);

        readings_.clear();
        processed_readings_.clear();
        can_frames_.clear();

        for(auto& [_, value] : reading_values_)
            value = kNoValue;
    }
};

} // eerie_leap::domain::sensor_domain::utilities
