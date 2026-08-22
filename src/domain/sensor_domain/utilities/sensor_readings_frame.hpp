#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <stdexcept>
#include <string>

#include <zephyr/spinlock.h>

#include "utilities/string/string_helpers.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::ReadingSource;

class SensorReadingsFrame {
private:
    std::unordered_map<uint32_t, SensorReading> isr_readings_;
    std::unordered_map<uint32_t, SensorReading> readings_;
    std::unordered_map<uint32_t, SensorReading> processed_readings_;
    std::unordered_map<uint32_t, float> reading_values_;
    mutable std::unordered_map<std::string, uint32_t> sensor_id_hash_map_;

    mutable k_sem processing_semaphore_;

    uint32_t GetSensorIdHash(const std::string& sensor_id) const {
        k_sem_take(&processing_semaphore_, K_FOREVER);

        if(!sensor_id_hash_map_.contains(sensor_id))
            sensor_id_hash_map_.emplace(sensor_id, StringHelpers::GetHash(sensor_id));

        uint32_t sensor_id_hash = sensor_id_hash_map_.at(sensor_id);

        k_sem_give(&processing_semaphore_);

        return sensor_id_hash;
    }

    void AddOrUpdateReadingIsr(SensorReading& reading) {
        uint32_t sensor_id_hash = reading.sensor->id_hash;

        if(isr_readings_.contains(sensor_id_hash))
            isr_readings_.erase(sensor_id_hash);
        isr_readings_.insert({ sensor_id_hash, reading });

        if(reading.status == ReadingStatus::PROCESSED && reading.value.has_value()) {
            reading_values_[sensor_id_hash] = reading.value.value();

            if(processed_readings_.contains(sensor_id_hash))
                processed_readings_.erase(sensor_id_hash);
            processed_readings_.insert({ sensor_id_hash, reading });
        }
    }

    void AddOrUpdateReadingProcessing(SensorReading& reading) {
        uint32_t sensor_id_hash = reading.sensor->id_hash;

        if(isr_readings_.contains(sensor_id_hash))
            isr_readings_.erase(sensor_id_hash);

        if(readings_.contains(sensor_id_hash))
            readings_.erase(sensor_id_hash);
        readings_.insert({ sensor_id_hash, reading });

        if(reading.status == ReadingStatus::PROCESSED && reading.value.has_value()) {
            reading_values_[sensor_id_hash] = reading.value.value();

            if(processed_readings_.contains(sensor_id_hash))
                processed_readings_.erase(sensor_id_hash);
            processed_readings_.insert({ sensor_id_hash, reading });
        }
    }

public:
    SensorReadingsFrame() {
        k_sem_init(&processing_semaphore_, 1, 1);
    }

    SensorReadingsFrame(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame(SensorReadingsFrame&&) = delete;
    SensorReadingsFrame& operator=(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame& operator=(SensorReadingsFrame&&) = delete;

    void AddOrUpdateReading(SensorReading& reading) {
        k_sem_take(&processing_semaphore_, K_FOREVER);

        if(reading.source == ReadingSource::ISR)
            AddOrUpdateReadingIsr(reading);
        else if(reading.source == ReadingSource::PROCESSING)
            AddOrUpdateReadingProcessing(reading);

        k_sem_give(&processing_semaphore_);
    }

    std::optional<SensorReading> TryGetIsrReading(const uint32_t sensor_id_hash) const {
        k_sem_take(&processing_semaphore_, K_FOREVER);

        std::optional<SensorReading> reading = std::nullopt;
        if(isr_readings_.contains(sensor_id_hash))
            reading.emplace(isr_readings_.at(sensor_id_hash));

        k_sem_give(&processing_semaphore_);

        return reading;
    }

    std::optional<SensorReading> TryGetIsrReading(const std::string& sensor_id) const {
        const uint32_t sensor_id_hash = GetSensorIdHash(sensor_id);

        return TryGetIsrReading(sensor_id_hash);
    }

    std::optional<SensorReading> TryGetReading(const uint32_t sensor_id_hash) const {
        k_sem_take(&processing_semaphore_, K_FOREVER);

        std::optional<SensorReading> reading = std::nullopt;
        if(readings_.contains(sensor_id_hash))
            reading.emplace(readings_.at(sensor_id_hash));

        k_sem_give(&processing_semaphore_);

        return reading;
    }

    std::optional<SensorReading> TryGetReading(const std::string& sensor_id) const {
        const uint32_t sensor_id_hash = GetSensorIdHash(sensor_id);

        return TryGetReading(sensor_id_hash);
    }

    std::optional<float> TryGetReadingValue(const uint32_t sensor_id_hash) const {
        k_sem_take(&processing_semaphore_, K_FOREVER);

        std::optional<float> reading = std::nullopt;
        if(reading_values_.contains(sensor_id_hash))
            reading.emplace(reading_values_.at(sensor_id_hash));

        k_sem_give(&processing_semaphore_);

        return reading;
    }

    std::optional<float> TryGetReadingValue(const std::string& sensor_id) const {
        const uint32_t sensor_id_hash = GetSensorIdHash(sensor_id);

        return TryGetReadingValue(sensor_id_hash);
    }

    float* GetReadingValuePtr(const std::string& sensor_id) {
        const uint32_t sensor_id_hash = GetSensorIdHash(sensor_id);

        k_sem_take(&processing_semaphore_, K_FOREVER);

        float* value_ptr = nullptr;
        if(reading_values_.contains(sensor_id_hash))
            value_ptr = &reading_values_.at(sensor_id_hash);

        k_sem_give(&processing_semaphore_);

        return value_ptr;
    }

    std::unordered_map<uint32_t, SensorReading> GetProcessedReadings() const {
        k_sem_take(&processing_semaphore_, K_FOREVER);
        std::unordered_map<uint32_t, SensorReading> readings(
            processed_readings_.begin(), processed_readings_.end());
        k_sem_give(&processing_semaphore_);

        return readings;
    }

    bool HasIsrReading(const uint32_t sensor_id_hash) {
        k_sem_take(&processing_semaphore_, K_FOREVER);
        bool result = isr_readings_.contains(sensor_id_hash);
        k_sem_give(&processing_semaphore_);

        return result;
    }

    bool HasReading(const uint32_t sensor_id_hash) {
        k_sem_take(&processing_semaphore_, K_FOREVER);
        bool result = readings_.contains(sensor_id_hash);
        k_sem_give(&processing_semaphore_);

        return result;
    }

    void ClearProcessedReadings() {
        k_sem_take(&processing_semaphore_, K_FOREVER);
        processed_readings_.clear();
        k_sem_give(&processing_semaphore_);
    }

    void ClearReadings() {
        k_sem_take(&processing_semaphore_, K_FOREVER);
        isr_readings_.clear();
        readings_.clear();
        processed_readings_.clear();
        reading_values_.clear();
        k_sem_give(&processing_semaphore_);
    }
};

} // eerie_leap::domain::sensor_domain::utilities
