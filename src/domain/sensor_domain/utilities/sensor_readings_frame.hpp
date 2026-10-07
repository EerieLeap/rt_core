#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <zephyr/kernel.h>

#include "utilities/string/string_helpers.h"
#include "subsys/threading/scoped_mutex.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::ReadingSource;

class SensorReadingsFrame {
private:
    std::unordered_map<uint32_t, SensorReading> isr_readings_;
    std::unordered_map<uint32_t, SensorReading> readings_;
    std::unordered_map<uint32_t, SensorReading> processed_readings_;
    // A value slot is created on first use and only ever rewritten afterwards, so a pointer handed
    // out by GetReadingValuePtr() stays valid for the lifetime of the frame. NaN marks "no value".
    std::unordered_map<uint32_t, float> reading_values_;
    // Sensors whose processed reading changed since the last TakeProcessedReadings().
    std::unordered_set<uint32_t> updated_sensor_id_hashes_;

    // Every method allocates while holding it, so the guard must release on a throw.
    mutable k_mutex lock_;

    static constexpr float kNoValue = std::numeric_limits<float>::quiet_NaN();

    static uint32_t GetSensorIdHash(std::string_view sensor_id) {
        return StringHelpers::GetHash(sensor_id);
    }

    static std::optional<float> ToOptional(float value) {
        return std::isnan(value) ? std::nullopt : std::optional<float>(value);
    }

    void StoreProcessedReading(uint32_t sensor_id_hash, const SensorReading& reading) {
        reading_values_[sensor_id_hash] = reading.value.value();

        if(processed_readings_.contains(sensor_id_hash))
            processed_readings_.erase(sensor_id_hash);
        processed_readings_.insert({ sensor_id_hash, reading });

        updated_sensor_id_hashes_.insert(sensor_id_hash);
    }

    void AddOrUpdateReadingIsr(SensorReading& reading) {
        uint32_t sensor_id_hash = reading.sensor->id_hash;

        if(isr_readings_.contains(sensor_id_hash))
            isr_readings_.erase(sensor_id_hash);
        isr_readings_.insert({ sensor_id_hash, reading });

        if(reading.status == ReadingStatus::PROCESSED && reading.value.has_value())
            StoreProcessedReading(sensor_id_hash, reading);
    }

    void AddOrUpdateReadingProcessing(SensorReading& reading) {
        uint32_t sensor_id_hash = reading.sensor->id_hash;

        if(isr_readings_.contains(sensor_id_hash))
            isr_readings_.erase(sensor_id_hash);

        if(readings_.contains(sensor_id_hash))
            readings_.erase(sensor_id_hash);
        readings_.insert({ sensor_id_hash, reading });

        if(reading.status == ReadingStatus::PROCESSED && reading.value.has_value())
            StoreProcessedReading(sensor_id_hash, reading);
    }

public:
    SensorReadingsFrame() {
        k_mutex_init(&lock_);
    }

    SensorReadingsFrame(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame(SensorReadingsFrame&&) = delete;
    SensorReadingsFrame& operator=(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame& operator=(SensorReadingsFrame&&) = delete;

    void AddOrUpdateReading(SensorReading& reading) {
        ScopedMutex guard(lock_);

        if(reading.source == ReadingSource::ISR)
            AddOrUpdateReadingIsr(reading);
        else if(reading.source == ReadingSource::PROCESSING)
            AddOrUpdateReadingProcessing(reading);
    }

    std::optional<SensorReading> TryGetIsrReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        std::optional<SensorReading> reading = std::nullopt;
        if(isr_readings_.contains(sensor_id_hash))
            reading.emplace(isr_readings_.at(sensor_id_hash));

        return reading;
    }

    std::optional<SensorReading> TryGetIsrReading(std::string_view sensor_id) const {
        return TryGetIsrReading(GetSensorIdHash(sensor_id));
    }

    std::optional<SensorReading> TryGetReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        std::optional<SensorReading> reading = std::nullopt;
        if(readings_.contains(sensor_id_hash))
            reading.emplace(readings_.at(sensor_id_hash));

        return reading;
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

    /** @brief Copies the latest processed reading of every sensor that has one. */
    std::unordered_map<uint32_t, SensorReading> GetProcessedReadings() const {
        ScopedMutex guard(lock_);

        return std::unordered_map<uint32_t, SensorReading>(processed_readings_.begin(), processed_readings_.end());
    }

    /**
     * @brief Copies the readings processed since the previous call and forgets them, in one step,
     *        so no update is lost between the copy and the reset.
     */
    std::unordered_map<uint32_t, SensorReading> TakeProcessedReadings() {
        ScopedMutex guard(lock_);

        std::unordered_map<uint32_t, SensorReading> readings;
        readings.reserve(updated_sensor_id_hashes_.size());

        for(const uint32_t sensor_id_hash : updated_sensor_id_hashes_) {
            const auto it = processed_readings_.find(sensor_id_hash);
            if(it != processed_readings_.end())
                readings.insert(*it);
        }

        updated_sensor_id_hashes_.clear();

        return readings;
    }

    bool HasIsrReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        return isr_readings_.contains(sensor_id_hash);
    }

    bool HasReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        return readings_.contains(sensor_id_hash);
    }

    void ClearProcessedReadings() {
        ScopedMutex guard(lock_);

        processed_readings_.clear();
        updated_sensor_id_hashes_.clear();
    }

    /** @brief Drops every reading. Value slots are kept, see GetReadingValuePtr(). */
    void ClearReadings() {
        ScopedMutex guard(lock_);

        isr_readings_.clear();
        readings_.clear();
        processed_readings_.clear();
        updated_sensor_id_hashes_.clear();

        for(auto& [_, value] : reading_values_)
            value = kNoValue;
    }
};

} // eerie_leap::domain::sensor_domain::utilities
