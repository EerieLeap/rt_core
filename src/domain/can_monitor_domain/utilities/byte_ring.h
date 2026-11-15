#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>

namespace eerie_leap::domain::can_monitor_domain::utilities {

/**
 * @brief A single-producer, single-consumer byte ring.
 *
 * The producer is the CAN receive thread (a meta-IRQ that can't take a lock or wait), the consumer
 * the sender thread. Head and tail are free-running counters: a store releases what was written or
 * read, a load of the other side's counter acquires it, and neither side ever waits.
 *
 * Producer calls: TryWrite(), Free(). Consumer calls: Size(), Peek(), Consume(). Capacity() is
 * constant. Clear() is for when neither side runs.
 */
class ByteRing {
private:
    std::pmr::memory_resource* memory_;
    uint8_t* data_;
    uint32_t capacity_;
    std::atomic<uint32_t> head_{ 0 };
    std::atomic<uint32_t> tail_{ 0 };

    ByteRing(std::pmr::memory_resource* memory, uint8_t* data, uint32_t capacity);

public:
    /** @brief The largest ring: free-running 32-bit counters tell full from empty up to 2^31. */
    static constexpr size_t MAX_CAPACITY = size_t{ 1 } << 31;

    /**
     * @brief Makes a ring of @p capacity bytes from @p memory.
     * @return nullptr when @p capacity is not a power of two in [1, 2^31] or the memory can't be had.
     */
    static std::unique_ptr<ByteRing> Create(size_t capacity, std::pmr::memory_resource* memory);

    ~ByteRing();

    ByteRing(const ByteRing&) = delete;
    ByteRing& operator=(const ByteRing&) = delete;
    ByteRing(ByteRing&&) = delete;
    ByteRing& operator=(ByteRing&&) = delete;

    [[nodiscard]] size_t Capacity() const {
        return capacity_;
    }

    // Producer.

    /** @brief Appends all of @p data, or none of it when it doesn't fit. */
    bool TryWrite(std::span<const uint8_t> data);

    /** @brief The bytes TryWrite() would take now. */
    [[nodiscard]] size_t Free() const;

    // Consumer.

    /** @brief The bytes written and not consumed yet. */
    [[nodiscard]] size_t Size() const;

    /**
     * @brief Copies out.size() bytes that start @p offset bytes past the oldest one, across the wrap.
     * @return false, copying nothing, when they are not all there.
     */
    bool Peek(size_t offset, std::span<uint8_t> out) const;

    /** @brief Drops the oldest @p count bytes; false, dropping nothing, when fewer are there. */
    bool Consume(size_t count);

    /** @brief Empties the ring. Only while neither side runs. */
    void Clear();
};

} // namespace eerie_leap::domain::can_monitor_domain::utilities
