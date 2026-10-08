#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "utilities/string/string_helpers.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/canbus/can_frame.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_slot.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::utilities {

using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorSlot;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingError;

// The latest reading of every configured sensor, shared between the processing pipeline and its
// consumers.
//
// A slot table: Configure() lays out one slot per sensor of a configuration generation, and
// nothing changes size until the next generation. Readings are packed into fixed records, so a
// commit or a snapshot is a few word copies under the lock and never allocates. Value slots have
// stable addresses for the generation, which is what expressions bind to. Consumers copy into
// buffers they own; the hash and ID overloads resolve the slot through a sorted index.
class SensorReadingsFrame {
private:
    // A reading without its identity and without the CAN frame pointer.
    struct Record {
        float value = 0.0F;
        float raw_value = 0.0F;
        float voltage = 0.0F;
        int64_t timestamp = 0;
        uint8_t flags = 0;
        ReadingStatus status = ReadingStatus::UNINITIALIZED;
        ReadingError error = ReadingError::NONE;
        ReadingSource source = ReadingSource::NONE;

        static constexpr uint8_t kHasValue = 1U << 0;
        static constexpr uint8_t kHasRawValue = 1U << 1;
        static constexpr uint8_t kHasVoltage = 1U << 2;
        static constexpr uint8_t kHasTimestamp = 1U << 3;

        static Record Pack(const SensorReading& reading) {
            Record record;

            if(reading.value.has_value()) { record.value = *reading.value; record.flags |= kHasValue; }
            if(reading.raw_value.has_value()) { record.raw_value = *reading.raw_value; record.flags |= kHasRawValue; }
            if(reading.voltage.has_value()) { record.voltage = *reading.voltage; record.flags |= kHasVoltage; }
            if(reading.timestamp.has_value()) {
                record.timestamp = reading.timestamp->time_since_epoch().count();
                record.flags |= kHasTimestamp;
            }

            record.status = reading.status;
            record.error = reading.error;
            record.source = reading.source;

            return record;
        }

        SensorReading Unpack(const Sensor* sensor) const {
            SensorReading reading(sensor);

            if(flags & kHasValue) reading.value = value;
            if(flags & kHasRawValue) reading.raw_value = raw_value;
            if(flags & kHasVoltage) reading.voltage = voltage;
            if(flags & kHasTimestamp) {
                reading.timestamp = std::chrono::system_clock::time_point(
                    std::chrono::system_clock::duration(timestamp));
            }

            reading.status = status;
            reading.error = error;
            reading.source = source;

            return reading;
        }
    };

    struct Slot {
        Record latest;
        Record processed;
        bool has_reading = false;
        bool has_processed = false;
        bool is_updated = false;             // Processed and not yet taken.
        uint16_t can_frame_index = SensorSlot::kInvalid;
    };

    struct IndexEntry {
        uint32_t sensor_id_hash;
        uint16_t slot;
    };

    std::vector<Slot> slots_;
    std::vector<const Sensor*> sensors_;     // By slot; owned by the generation that configured the frame.
    std::vector<float> values_;              // Latest processed value by slot, NaN for none. Addresses are stable.
    std::vector<IndexEntry> index_;          // Sorted by hash.
    std::vector<CanFrame> can_frames_;       // One per CANBUS_RAW sensor.
    uint32_t generation_ = 0;

    // Holds across the record copies; nothing allocates while it is held after Configure().
    mutable k_mutex lock_;

    static constexpr float kNoValue = std::numeric_limits<float>::quiet_NaN();

    static uint32_t GetSensorIdHash(std::string_view sensor_id) {
        return StringHelpers::GetHash(sensor_id);
    }

    static std::optional<float> ToOptional(float value) {
        return std::isnan(value) ? std::nullopt : std::optional<float>(value);
    }

    std::optional<SensorSlot> FindSlotLocked(uint32_t sensor_id_hash) const {
        const auto it = std::lower_bound(index_.begin(), index_.end(), sensor_id_hash,
            [](const IndexEntry& entry, uint32_t hash) { return entry.sensor_id_hash < hash; });

        if(it == index_.end() || it->sensor_id_hash != sensor_id_hash)
            return std::nullopt;

        return SensorSlot{it->slot};
    }

    bool IsValidLocked(SensorSlot slot) const {
        return slot.index < slots_.size();
    }

    void ResetLocked() {
        for(auto& slot : slots_) {
            slot.latest = Record{};
            slot.processed = Record{};
            slot.has_reading = false;
            slot.has_processed = false;
            slot.is_updated = false;
        }

        std::fill(values_.begin(), values_.end(), kNoValue);

        for(auto& can_frame : can_frames_)
            can_frame = CanFrame{};
    }

public:
    SensorReadingsFrame() {
        k_mutex_init(&lock_);
    }

    SensorReadingsFrame(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame(SensorReadingsFrame&&) = delete;
    SensorReadingsFrame& operator=(const SensorReadingsFrame&) = delete;
    SensorReadingsFrame& operator=(SensorReadingsFrame&&) = delete;

    /**
     * @brief Lays out one slot per sensor, in the given order, and drops every reading.
     *
     * The sensors must outlive the frame's use of them, until the next Configure(). Readings of
     * sensors that are not configured are ignored.
     * @return The new generation number.
     */
    uint32_t Configure(std::span<const std::shared_ptr<Sensor>> sensors) {
        ScopedMutex guard(lock_);

        const size_t count = sensors.size();

        slots_.assign(count, Slot{});
        sensors_.resize(count);
        values_.assign(count, kNoValue);
        index_.resize(count);

        size_t can_frame_count = 0;
        for(size_t i = 0; i < count; i++) {
            sensors_[i] = sensors[i].get();
            index_[i] = IndexEntry{sensors[i]->id_hash, static_cast<uint16_t>(i)};

            if(sensors[i]->configuration.GetTraits().source == models::SensorSourceKind::CAN_FRAME)
                slots_[i].can_frame_index = static_cast<uint16_t>(can_frame_count++);
        }

        std::sort(index_.begin(), index_.end(),
            [](const IndexEntry& a, const IndexEntry& b) { return a.sensor_id_hash < b.sensor_id_hash; });

        can_frames_.assign(can_frame_count, CanFrame{});

        return ++generation_;
    }

    /** @return The number of Configure() calls so far; consumers caching slots compare it. */
    [[nodiscard]] uint32_t GetGeneration() const {
        ScopedMutex guard(lock_);

        return generation_;
    }

    [[nodiscard]] size_t GetSlotCount() const {
        ScopedMutex guard(lock_);

        return slots_.size();
    }

    [[nodiscard]] std::optional<SensorSlot> FindSlot(uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        return FindSlotLocked(sensor_id_hash);
    }

    [[nodiscard]] std::optional<SensorSlot> FindSlot(std::string_view sensor_id) const {
        return FindSlot(GetSensorIdHash(sensor_id));
    }

    /**
     * @brief Address of the slot's latest processed value, for evaluators that read it directly.
     *
     * Holds NaN until the sensor has a processed value. Stable until the next Configure().
     */
    float* GetValueAddress(SensorSlot slot) {
        ScopedMutex guard(lock_);

        return IsValidLocked(slot) ? &values_[slot.index] : nullptr;
    }

    /** @brief Same by sensor ID; null for a sensor that is not configured. */
    float* GetReadingValuePtr(std::string_view sensor_id) {
        ScopedMutex guard(lock_);

        const auto slot = FindSlotLocked(GetSensorIdHash(sensor_id));

        return slot.has_value() ? &values_[slot->index] : nullptr;
    }

    /** @brief Whether every slot has a processed value, in one lock. */
    [[nodiscard]] bool AreValuesAvailable(std::span<const SensorSlot> slots) const {
        ScopedMutex guard(lock_);

        for(const auto slot : slots) {
            if(!IsValidLocked(slot) || std::isnan(values_[slot.index]))
                return false;
        }

        return true;
    }

    /**
     * @brief Stores @p reading as its sensor's latest state. A reading without a source, or of a
     *        sensor that is not configured, is ignored.
     *
     * A PROCESSED reading also becomes the sensor's latest processed reading and, when it has a
     * value, its latest value. A CAN frame referenced by the reading is copied into the frame store.
     * @return Whether the reading was stored.
     */
    bool AddOrUpdateReading(const SensorReading& reading) {
        if(reading.source == ReadingSource::NONE)
            return false;

        ScopedMutex guard(lock_);

        const auto slot_index = FindSlotLocked(reading.sensor_id_hash);
        if(!slot_index.has_value())
            return false;

        Slot& slot = slots_[slot_index->index];

        slot.latest = Record::Pack(reading);
        slot.has_reading = true;

        if(reading.can_frame != nullptr && slot.can_frame_index != SensorSlot::kInvalid)
            can_frames_[slot.can_frame_index] = *reading.can_frame;

        if(reading.status != ReadingStatus::PROCESSED)
            return true;

        slot.processed = slot.latest;
        slot.has_processed = true;
        slot.is_updated = true;

        if(reading.value.has_value())
            values_[slot_index->index] = reading.value.value();

        return true;
    }

    std::optional<SensorReading> TryGetReading(SensorSlot slot) const {
        ScopedMutex guard(lock_);

        if(!IsValidLocked(slot) || !slots_[slot.index].has_reading)
            return std::nullopt;

        return slots_[slot.index].latest.Unpack(sensors_[slot.index]);
    }

    std::optional<SensorReading> TryGetReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        const auto slot = FindSlotLocked(sensor_id_hash);
        if(!slot.has_value() || !slots_[slot->index].has_reading)
            return std::nullopt;

        return slots_[slot->index].latest.Unpack(sensors_[slot->index]);
    }

    std::optional<SensorReading> TryGetReading(std::string_view sensor_id) const {
        return TryGetReading(GetSensorIdHash(sensor_id));
    }

    std::optional<float> TryGetReadingValue(SensorSlot slot) const {
        ScopedMutex guard(lock_);

        return IsValidLocked(slot) ? ToOptional(values_[slot.index]) : std::nullopt;
    }

    std::optional<float> TryGetReadingValue(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        const auto slot = FindSlotLocked(sensor_id_hash);

        return slot.has_value() ? ToOptional(values_[slot->index]) : std::nullopt;
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
            const auto slot = FindSlotLocked(sensor_id_hashes[i]);
            values[i] = slot.has_value() ? ToOptional(values_[slot->index]) : std::nullopt;
        }
    }

    /** @brief Copies the latest CAN frame stored for a CANBUS_RAW sensor. */
    bool TryGetCanFrame(const uint32_t sensor_id_hash, CanFrame& can_frame) const {
        ScopedMutex guard(lock_);

        const auto slot = FindSlotLocked(sensor_id_hash);
        if(!slot.has_value())
            return false;

        const auto can_frame_index = slots_[slot->index].can_frame_index;
        if(can_frame_index == SensorSlot::kInvalid || !slots_[slot->index].has_reading)
            return false;

        can_frame = can_frames_[can_frame_index];

        return true;
    }

    [[nodiscard]] size_t GetProcessedReadingCount() const {
        ScopedMutex guard(lock_);

        size_t count = 0;
        for(const auto& slot : slots_)
            count += slot.has_processed ? 1 : 0;

        return count;
    }

    /**
     * @brief Copies the latest processed reading of every sensor into @p readings.
     * @return The number copied, at most readings.size().
     */
    size_t SnapshotProcessedReadings(std::span<SensorReading> readings) const {
        ScopedMutex guard(lock_);

        size_t count = 0;
        for(size_t i = 0; i < slots_.size() && count < readings.size(); i++) {
            if(slots_[i].has_processed)
                readings[count++] = slots_[i].processed.Unpack(sensors_[i]);
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
        for(size_t i = 0; i < slots_.size() && count < readings.size(); i++) {
            if(!slots_[i].is_updated)
                continue;

            readings[count++] = slots_[i].processed.Unpack(sensors_[i]);
            slots_[i].is_updated = false;
        }

        return count;
    }

    bool HasReading(const uint32_t sensor_id_hash) const {
        ScopedMutex guard(lock_);

        const auto slot = FindSlotLocked(sensor_id_hash);

        return slot.has_value() && slots_[slot->index].has_reading;
    }

    void ClearProcessedReadings() {
        ScopedMutex guard(lock_);

        for(auto& slot : slots_) {
            slot.processed = Record{};
            slot.has_processed = false;
            slot.is_updated = false;
        }
    }

    /** @brief Drops every reading and value; the slots and their addresses stay. */
    void ClearReadings() {
        ScopedMutex guard(lock_);

        ResetLocked();
    }
};

} // eerie_leap::domain::sensor_domain::utilities
