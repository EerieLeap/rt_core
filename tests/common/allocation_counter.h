#pragma once

// Replaces the global allocation functions of the test binary so a test can count every heap
// allocation in the process. Replacement functions cannot be inline, so exactly one translation
// unit per test binary includes this header; a second one fails to link.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace rt_core_test {

namespace allocation_counter_detail {

std::atomic<size_t> allocation_count{ 0 };

void* CountedAllocate(size_t size) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);

    void* pointer = std::malloc(size == 0 ? 1 : size);
    if(pointer == nullptr)
        throw std::bad_alloc();

    return pointer;
}

void* CountedAllocate(size_t size, const std::nothrow_t&) noexcept {
    allocation_count.fetch_add(1, std::memory_order_relaxed);

    return std::malloc(size == 0 ? 1 : size);
}

} // namespace allocation_counter_detail

/** Allocations through operator new since the program started. */
size_t GetAllocationCount() {
    return allocation_counter_detail::allocation_count.load(std::memory_order_relaxed);
}

} // namespace rt_core_test

void* operator new(size_t size) {
    return rt_core_test::allocation_counter_detail::CountedAllocate(size);
}
void* operator new[](size_t size) {
    return rt_core_test::allocation_counter_detail::CountedAllocate(size);
}
void* operator new(size_t size, const std::nothrow_t& tag) noexcept {
    return rt_core_test::allocation_counter_detail::CountedAllocate(size, tag);
}
void* operator new[](size_t size, const std::nothrow_t& tag) noexcept {
    return rt_core_test::allocation_counter_detail::CountedAllocate(size, tag);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}
