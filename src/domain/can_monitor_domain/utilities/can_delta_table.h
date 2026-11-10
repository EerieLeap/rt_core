#pragma once

// Without EERIE_LEAP_CAN_MONITOR_DELTA this header is empty, and so is can_delta_table.cpp's build:
// CanRecordWriter only forward-declares the class.
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA

    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <memory_resource>
    #include <optional>
    #include <span>

    #include "domain/can_monitor_domain/utilities/can_record.h"
    #include "domain/can_monitor_domain/utilities/can_record_reader.h"
    #include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

/**
 * @brief The last payload of up to `capacity` identifiers, and the encoder that sends a repeat as
 * its changed bytes (record kind 1, §4.3).
 *
 * An entry is a (ID, IDE) and the payload last sent for it, 1 to 64 bytes. Entries are added while
 * there is room and never evicted, so an ID that found the table full goes whole for good (until
 * Reset()). Lookup is a hash of the key into an open-addressing index (linear probing, at most half
 * full), so it costs O(1) on the receive thread. The storage, about 69 bytes an entry, comes from
 * one allocation of the memory resource.
 *
 * The app keeps the same table (CanDeltaDecoder here, for the tests): both sides apply the same
 * rules, so they hold the same payloads as long as both reset at a gap or tap record.
 */
class CanDeltaTable {
public:
    static constexpr size_t MAX_PAYLOAD_SIZE = CAN_MAX_PAYLOAD_SIZE;
    /** @brief The entries' index is 16 bits. */
    static constexpr size_t MAX_CAPACITY = 4096;

private:
    std::pmr::memory_resource* memory_;
    void* block_;
    size_t block_size_;

    uint8_t* payloads_;
    uint32_t* keys_;
    uint16_t* index_;
    uint8_t* lengths_;

    size_t capacity_;
    size_t slots_;
    uint32_t shift_;
    size_t count_ = 0;

    CanDeltaTable(std::pmr::memory_resource* memory, void* block, size_t block_size, size_t capacity);

    [[nodiscard]] size_t FindSlot(uint32_t key) const;

public:
    /**
     * @brief Makes a table for @p capacity identifiers.
     * @return nullptr when @p capacity is not in [1, MAX_CAPACITY] or the memory can't be had.
     */
    static std::unique_ptr<CanDeltaTable> Create(
        size_t capacity = CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA_IDS,
        std::pmr::memory_resource* memory = eerie_leap::utilities::memory::Mrm::GetExtPmr()
    );

    ~CanDeltaTable();

    CanDeltaTable(const CanDeltaTable&) = delete;
    CanDeltaTable& operator=(const CanDeltaTable&) = delete;
    CanDeltaTable(CanDeltaTable&&) = delete;
    CanDeltaTable& operator=(CanDeltaTable&&) = delete;

    [[nodiscard]] size_t Capacity() const {
        return capacity_;
    }
    /** @brief The identifiers held. */
    [[nodiscard]] size_t Size() const {
        return count_;
    }

    /** @brief Forgets every payload: the next frame of every ID goes whole. */
    void Reset();

    /** @brief The payload held for an identifier; empty if there is none. */
    [[nodiscard]] std::span<const uint8_t> Lookup(uint32_t id, bool ide) const;

    /**
     * @brief Sets the payload held for an identifier (of any length from 1 to 64), adding it if there is room.
     * @return false when the identifier has no entry and the table is full, or the payload's size is not 1 to 64.
     */
    bool Store(uint32_t id, bool ide, std::span<const uint8_t> payload);

    /**
     * @brief Encodes a data frame: as changed bytes (kind 1) when the identifier has an entry with a
     * payload of the same length and that is smaller, otherwise whole (kind 0); then the entry
     * takes the frame's payload (added if there is room). Remote frames and empty payloads go whole
     * and leave the table alone.
     * @return The record's size, or 0 (the table untouched) when @p out is too small or the frame is malformed.
     */
    size_t Encode(std::span<uint8_t> out, uint64_t dt_us, const CanRecordFrame& frame);
};

/**
 * @brief Turns the kind 0 and 1 records of a stream back into whole payloads, as the app does.
 *
 * Holds the same table with the same rules as the encoder's; a gap or tap record empties it.
 */
class CanDeltaDecoder {
private:
    CanDeltaTable& table_;

public:
    explicit CanDeltaDecoder(CanDeltaTable& table) :
        table_(table) {}

    /**
     * @brief Applies one record.
     *
     * - Kind 0 and 1: writes the whole payload to @p payload_out and returns its length.
     * - Kind 4 (gap) and 7 (tap): empties the table and returns 0.
     * - Any other kind: returns 0 and changes nothing.
     *
     * @return nothing when a kind 1 has no entry of its length to apply to, or @p payload_out is too small.
     */
    std::optional<size_t> Apply(const CanRecord& record, std::span<uint8_t> payload_out);
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
