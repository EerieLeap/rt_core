#include "domain/can_monitor_domain/utilities/byte_ring.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace eerie_leap::domain::can_monitor_domain::utilities {

namespace {

constexpr size_t RING_ALIGNMENT = 8;

} // namespace

ByteRing::ByteRing(std::pmr::memory_resource* memory, uint8_t* data, uint32_t capacity) :
    memory_(memory),
    data_(data),
    capacity_(capacity) {}

std::unique_ptr<ByteRing> ByteRing::Create(size_t capacity, std::pmr::memory_resource* memory) {
    if(memory == nullptr || capacity == 0 || capacity > MAX_CAPACITY || (capacity & (capacity - 1)) != 0)
        return nullptr;

    uint8_t* data = nullptr;
    try {
        data = static_cast<uint8_t*>(memory->allocate(capacity, RING_ALIGNMENT));
    } catch(const std::bad_alloc&) {
        return nullptr;
    }

    if(data == nullptr)
        return nullptr;

    // The constructor is private, so make_unique can't reach it.
    std::unique_ptr<ByteRing> ring(new(std::nothrow) ByteRing(memory, data, static_cast<uint32_t>(capacity)));
    if(!ring)
        memory->deallocate(data, capacity, RING_ALIGNMENT);

    return ring;
}

ByteRing::~ByteRing() {
    memory_->deallocate(data_, capacity_, RING_ALIGNMENT);
}

bool ByteRing::TryWrite(std::span<const uint8_t> data) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    const uint32_t free = capacity_ - (head - tail);

    if(data.size() > free)
        return false;

    if(data.empty())
        return true;

    const auto count = static_cast<uint32_t>(data.size());
    const uint32_t offset = head & (capacity_ - 1);
    const uint32_t first = std::min(count, capacity_ - offset);

    std::memcpy(data_ + offset, data.data(), first);
    if(count > first)
        std::memcpy(data_, data.data() + first, count - first);

    head_.store(head + count, std::memory_order_release);

    return true;
}

size_t ByteRing::Free() const {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);

    return capacity_ - (head - tail);
}

size_t ByteRing::Size() const {
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t tail = tail_.load(std::memory_order_relaxed);

    return head - tail;
}

bool ByteRing::Peek(size_t offset, std::span<uint8_t> out) const {
    const size_t size = Size();
    if(offset > size || out.size() > size - offset)
        return false;

    if(out.empty())
        return true;

    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t start = (tail + static_cast<uint32_t>(offset)) & (capacity_ - 1);
    const auto count = static_cast<uint32_t>(out.size());
    const uint32_t first = std::min(count, capacity_ - start);

    std::memcpy(out.data(), data_ + start, first);
    if(count > first)
        std::memcpy(out.data() + first, data_, count - first);

    return true;
}

bool ByteRing::Consume(size_t count) {
    if(count > Size())
        return false;

    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    tail_.store(tail + static_cast<uint32_t>(count), std::memory_order_release);

    return true;
}

void ByteRing::Clear() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities
