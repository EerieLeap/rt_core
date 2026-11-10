#include <zephyr/ztest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <vector>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/byte_ring.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_byte_ring, NULL, NULL, NULL, NULL, NULL);

namespace {

/** @brief Counts what is out, and can be made to fail. */
class CountingResource : public std::pmr::memory_resource {
public:
    size_t outstanding = 0;
    size_t allocations = 0;
    bool fail = false;

private:
    void* do_allocate(size_t bytes, size_t align) override {
        if(fail)
            throw std::bad_alloc();

        outstanding += bytes;
        ++allocations;

        return std::pmr::new_delete_resource()->allocate(bytes, align);
    }

    void do_deallocate(void* ptr, size_t bytes, size_t align) override {
        outstanding -= bytes;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, align);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::vector<uint8_t> Bytes(uint8_t first, size_t count) {
    std::vector<uint8_t> bytes(count);
    for(size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<uint8_t>(first + i);

    return bytes;
}

} // namespace

ZTEST(can_monitor_byte_ring, test_CreateNeedsAPowerOfTwoAndMemory) {
    auto* memory = std::pmr::get_default_resource();

    zassert_is_null(ByteRing::Create(0, memory).get());
    zassert_is_null(ByteRing::Create(3, memory).get());
    zassert_is_null(ByteRing::Create(1000, memory).get());
    zassert_is_null(ByteRing::Create(1025, memory).get());
    zassert_is_null(ByteRing::Create(1024, nullptr).get());
    zassert_is_null(ByteRing::Create((size_t{ 1 } << 31) + 1, memory).get());

    for(const size_t capacity : { size_t{ 1 }, size_t{ 2 }, size_t{ 1024 }, size_t{ 65536 } }) {
        auto ring = ByteRing::Create(capacity, memory);
        zassert_not_null(ring.get());
        zassert_equal(ring->Capacity(), capacity);
        zassert_equal(ring->Size(), 0U);
        zassert_equal(ring->Free(), capacity);
    }
}

ZTEST(can_monitor_byte_ring, test_CreateFailsWhenTheMemoryCannotBeHad) {
    CountingResource memory;
    memory.fail = true;

    zassert_is_null(ByteRing::Create(1024, &memory).get());
    zassert_equal(memory.outstanding, 0U);
}

ZTEST(can_monitor_byte_ring, test_ItFreesItsMemory) {
    CountingResource memory;

    {
        auto ring = ByteRing::Create(4096, &memory);
        zassert_not_null(ring.get());
        zassert_equal(memory.outstanding, 4096U);
    }

    zassert_equal(memory.outstanding, 0U);
}

ZTEST(can_monitor_byte_ring, test_WritePeekConsume) {
    auto ring = MakeRing(16);

    const auto data = Bytes(10, 6);
    zassert_true(ring->TryWrite(data));
    zassert_equal(ring->Size(), 6U);
    zassert_equal(ring->Free(), 10U);

    std::array<uint8_t, 6> out{};
    zassert_true(ring->Peek(0, out));
    zassert_true(std::equal(out.begin(), out.end(), data.begin()));

    // A peek takes nothing out.
    zassert_equal(ring->Size(), 6U);

    std::array<uint8_t, 2> middle{};
    zassert_true(ring->Peek(3, middle));
    zassert_equal(middle[0], 13U);
    zassert_equal(middle[1], 14U);

    zassert_true(ring->Consume(2));
    zassert_equal(ring->Size(), 4U);
    zassert_equal(ring->Free(), 12U);
    zassert_true(ring->Peek(0, middle));
    zassert_equal(middle[0], 12U);
}

ZTEST(can_monitor_byte_ring, test_PeekAndConsumeRefuseMoreThanIsThere) {
    auto ring = MakeRing(16);
    zassert_true(ring->TryWrite(Bytes(0, 5)));

    std::array<uint8_t, 6> out{};
    zassert_false(ring->Peek(0, out));
    zassert_false(ring->Peek(1, std::span<uint8_t>(out).first(5)));
    zassert_false(ring->Peek(6, std::span<uint8_t>(out).first(0)));
    zassert_true(ring->Peek(5, std::span<uint8_t>(out).first(0)));
    zassert_true(ring->Peek(1, std::span<uint8_t>(out).first(4)));

    zassert_false(ring->Consume(6));
    zassert_equal(ring->Size(), 5U);
    zassert_true(ring->Consume(5));
    zassert_equal(ring->Size(), 0U);
}

ZTEST(can_monitor_byte_ring, test_ItWrapsAround) {
    auto ring = MakeRing(16);

    zassert_true(ring->TryWrite(Bytes(0, 12)));
    zassert_true(ring->Consume(12));

    // 10 bytes at offset 12: 4 before the end, 6 after.
    const auto data = Bytes(100, 10);
    zassert_true(ring->TryWrite(data));
    zassert_equal(ring->Size(), 10U);

    std::array<uint8_t, 10> out{};
    zassert_true(ring->Peek(0, out));
    zassert_true(std::equal(out.begin(), out.end(), data.begin()));

    // A peek that starts before the end and runs past it, and one that starts after.
    std::array<uint8_t, 4> part{};
    zassert_true(ring->Peek(2, part));
    zassert_equal(part[0], 102U);
    zassert_equal(part[3], 105U);
    zassert_true(ring->Peek(7, std::span<uint8_t>(part).first(3)));
    zassert_equal(part[0], 107U);
    zassert_equal(part[2], 109U);
}

ZTEST(can_monitor_byte_ring, test_ManyLapsKeepTheBytesInOrder) {
    auto ring = MakeRing(32);

    uint8_t next_write = 0;
    uint8_t next_read = 0;
    for(size_t round = 0; round < 500; ++round) {
        const size_t count = (round % 7) + 1;
        std::vector<uint8_t> data(count);
        for(auto& byte : data)
            byte = next_write++;
        zassert_true(ring->TryWrite(data));

        if(ring->Size() > 20) {
            std::vector<uint8_t> out(ring->Size());
            zassert_true(ring->Peek(0, out));
            for(const uint8_t byte : out)
                zassert_equal(byte, next_read++);
            zassert_true(ring->Consume(out.size()));
        }
    }
}

ZTEST(can_monitor_byte_ring, test_ItIsFullAllOrNothing) {
    auto ring = MakeRing(16);

    zassert_true(ring->TryWrite(Bytes(0, 16)));
    zassert_equal(ring->Free(), 0U);
    zassert_false(ring->TryWrite(Bytes(0, 1)));
    zassert_equal(ring->Size(), 16U);

    zassert_true(ring->Consume(4));
    zassert_equal(ring->Free(), 4U);

    // Five do not fit in four, and none of them goes in.
    zassert_false(ring->TryWrite(Bytes(50, 5)));
    zassert_equal(ring->Size(), 12U);
    zassert_equal(ring->Free(), 4U);

    zassert_true(ring->TryWrite(Bytes(50, 4)));
    zassert_equal(ring->Free(), 0U);

    std::array<uint8_t, 16> out{};
    zassert_true(ring->Peek(0, out));
    zassert_equal(out[0], 4U);
    zassert_equal(out[11], 15U);
    zassert_equal(out[12], 50U);
    zassert_equal(out[15], 53U);
}

ZTEST(can_monitor_byte_ring, test_EmptyWritesAndAOneByteRing) {
    auto ring = MakeRing(1);

    zassert_true(ring->TryWrite(std::span<const uint8_t>()));
    zassert_true(ring->TryWrite(Bytes(9, 1)));
    zassert_false(ring->TryWrite(Bytes(9, 1)));

    std::array<uint8_t, 1> out{};
    zassert_true(ring->Peek(0, out));
    zassert_equal(out[0], 9U);
    zassert_true(ring->Consume(1));
    zassert_true(ring->TryWrite(Bytes(8, 1)));
}

ZTEST(can_monitor_byte_ring, test_ClearEmptiesIt) {
    auto ring = MakeRing(16);
    zassert_true(ring->TryWrite(Bytes(0, 11)));
    zassert_true(ring->Consume(3));

    ring->Clear();

    zassert_equal(ring->Size(), 0U);
    zassert_equal(ring->Free(), 16U);
    zassert_true(ring->TryWrite(Bytes(0, 16)));
}
