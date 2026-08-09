#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
#include <zephyr/ztest.h>

#include "utilities/memory/heap_allocator.h"

using eerie_leap::utilities::memory::ExtString;
using eerie_leap::utilities::memory::ExtVector;
using eerie_leap::utilities::memory::HeapAllocator;
using eerie_leap::utilities::memory::ext_unique_ptr;
using eerie_leap::utilities::memory::make_shared_ext;
using eerie_leap::utilities::memory::make_unique_ext;

namespace {

struct Tracked {
    static int live;
    static int constructed;

    int value;

    explicit Tracked(int v = 0) : value(v) {
        ++live;
        ++constructed;
    }

    Tracked(const Tracked& other) : value(other.value) {
        ++live;
        ++constructed;
    }

    ~Tracked() { --live; }
};

int Tracked::live = 0;
int Tracked::constructed = 0;

struct ThrowingCtor {
    ThrowingCtor() { throw std::runtime_error("boom"); }
};

void ResetTracked() {
    Tracked::live = 0;
    Tracked::constructed = 0;
}

} // namespace

ZTEST_SUITE(heap_allocator, NULL, NULL, NULL, NULL, NULL);

ZTEST(heap_allocator, test_allocate_and_deallocate) {
    HeapAllocator<uint32_t> allocator;

    uint32_t* block = allocator.allocate(8);

    zassert_not_null(block);
    zassert_equal(reinterpret_cast<uintptr_t>(block) % alignof(uint32_t), 0);

    for (uint32_t i = 0; i < 8; ++i)
        block[i] = i * 7;
    for (uint32_t i = 0; i < 8; ++i)
        zassert_equal(block[i], i * 7);

    allocator.deallocate(block, 8);
}

ZTEST(heap_allocator, test_allocate_zero_sized_request) {
    HeapAllocator<uint8_t> allocator;

    bool threw = false;
    uint8_t* block = nullptr;
    try {
        block = allocator.allocate(0);
    } catch (const std::bad_alloc&) {
        threw = true;
    }

    if (!threw)
        allocator.deallocate(block, 0);
}

ZTEST(heap_allocator, test_allocate_failure_throws_bad_alloc) {
    HeapAllocator<uint8_t> allocator;

    bool threw = false;
    uint8_t* block = nullptr;
    try {
        block = allocator.allocate(64 * 1024 * 1024);
    } catch (const std::bad_alloc&) {
        threw = true;
    }

    if (!threw)
        allocator.deallocate(block, 64 * 1024 * 1024);

    zassert_true(threw, "oversized allocation should throw std::bad_alloc");
}

ZTEST(heap_allocator, test_reallocate_preserves_contents) {
    HeapAllocator<uint32_t> allocator;

    uint32_t* block = allocator.allocate(4);
    for (uint32_t i = 0; i < 4; ++i)
        block[i] = i + 1;

    block = allocator.reallocate(block, 4, 16);

    zassert_not_null(block);
    for (uint32_t i = 0; i < 4; ++i)
        zassert_equal(block[i], i + 1);

    allocator.deallocate(block, 16);
}

ZTEST(heap_allocator, test_reallocate_shrinks) {
    HeapAllocator<uint32_t> allocator;

    uint32_t* block = allocator.allocate(16);
    for (uint32_t i = 0; i < 16; ++i)
        block[i] = i;

    block = allocator.reallocate(block, 16, 2);

    zassert_not_null(block);
    zassert_equal(block[0], 0);
    zassert_equal(block[1], 1);

    allocator.deallocate(block, 2);
}

ZTEST(heap_allocator, test_reallocate_null_allocates) {
    HeapAllocator<uint32_t> allocator;

    uint32_t* block = allocator.reallocate(nullptr, 0, 4);

    zassert_not_null(block);
    block[3] = 42;
    zassert_equal(block[3], 42);

    allocator.deallocate(block, 4);
}

ZTEST(heap_allocator, test_is_stateless_and_always_equal) {
    HeapAllocator<uint32_t> a;
    HeapAllocator<uint32_t> b;
    HeapAllocator<uint8_t> rebound(a);

    zassert_true(a == b);
    zassert_false(a != b);
    zassert_true(a == rebound);

    // Blocks are interchangeable between instances because the allocator is stateless.
    uint32_t* block = a.allocate(4);
    b.deallocate(block, 4);
}

ZTEST(heap_allocator, test_works_with_std_vector) {
    std::vector<int, HeapAllocator<int>> values;

    for (int i = 0; i < 256; ++i)
        values.push_back(i);

    zassert_equal(values.size(), 256);
    zassert_equal(values.front(), 0);
    zassert_equal(values.back(), 255);
}

ZTEST(heap_allocator, test_ExtVector) {
    ExtVector bytes;

    for (uint8_t i = 0; i < 64; ++i)
        bytes.push_back(i);

    zassert_equal(bytes.size(), 64);
    zassert_equal(bytes[0], 0);
    zassert_equal(bytes[63], 63);

    bytes.clear();
    zassert_true(bytes.empty());
}

ZTEST(heap_allocator, test_ExtString) {
    // Long enough to defeat the small-string optimisation and force an allocation.
    ExtString text("a string long enough to require heap storage");

    text.append(" plus more");

    zassert_equal(text.size(), strlen("a string long enough to require heap storage plus more"));
    zassert_true(text.compare("a string long omit") != 0);
    zassert_mem_equal(text.data(), "a string long enough", 20);
}

ZTEST(heap_allocator, test_make_shared_ext) {
    ResetTracked();

    {
        auto tracked = make_shared_ext<Tracked>(7);

        zassert_not_null(tracked.get());
        zassert_equal(tracked->value, 7);
        zassert_equal(Tracked::live, 1);

        auto copy = tracked;
        zassert_equal(copy.use_count(), 2);
        zassert_equal(Tracked::live, 1);
    }

    zassert_equal(Tracked::live, 0);
}

ZTEST(heap_allocator, test_make_unique_ext) {
    ResetTracked();

    {
        ext_unique_ptr<Tracked> tracked = make_unique_ext<Tracked>(11);

        zassert_not_null(tracked.get());
        zassert_equal(tracked->value, 11);
        zassert_equal(Tracked::live, 1);
    }

    zassert_equal(Tracked::live, 0);
}

ZTEST(heap_allocator, test_make_unique_ext_is_movable) {
    ResetTracked();

    auto source = make_unique_ext<Tracked>(3);
    auto target = std::move(source);

    zassert_is_null(source.get());
    zassert_equal(target->value, 3);
    zassert_equal(Tracked::live, 1);

    target.reset();
    zassert_equal(Tracked::live, 0);
}

ZTEST(heap_allocator, test_make_unique_ext_rethrows_constructor_failure) {
    bool threw = false;
    try {
        auto value = make_unique_ext<ThrowingCtor>();
        (void)value;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(heap_allocator, test_ext_deleter_ignores_null) {
    ext_unique_ptr<Tracked> empty;

    zassert_is_null(empty.get());
}
