#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource.h"

using eerie_leap::utilities::memory::ExtMemoryResource;

ZTEST_SUITE(ext_memory_resource, NULL, NULL, NULL, NULL, NULL);

ZTEST(ext_memory_resource, test_allocate_and_deallocate) {
    ExtMemoryResource resource;

    void* block = resource.allocate(128, alignof(uint64_t));

    zassert_not_null(block);
    zassert_equal(reinterpret_cast<uintptr_t>(block) % alignof(uint64_t), 0);

    memset(block, 0xA5, 128);
    zassert_equal(static_cast<uint8_t*>(block)[127], 0xA5);

    resource.deallocate(block, 128, alignof(uint64_t));
}

ZTEST(ext_memory_resource, test_allocations_do_not_overlap) {
    ExtMemoryResource resource;

    auto* first = static_cast<uint8_t*>(resource.allocate(64, 8));
    auto* second = static_cast<uint8_t*>(resource.allocate(64, 8));

    zassert_true(first != second);

    memset(first, 0x11, 64);
    memset(second, 0x22, 64);

    zassert_equal(first[0], 0x11);
    zassert_equal(second[0], 0x22);

    resource.deallocate(second, 64, 8);
    resource.deallocate(first, 64, 8);
}

ZTEST(ext_memory_resource, test_is_equal_is_identity_based) {
    ExtMemoryResource first;
    ExtMemoryResource second;

    zassert_true(first.is_equal(first));
    zassert_false(first.is_equal(second));
    zassert_false(first.is_equal(*std::pmr::new_delete_resource()));
    zassert_true(first == first);
    zassert_true(first != second);
}

ZTEST(ext_memory_resource, test_backs_pmr_vector) {
    ExtMemoryResource resource;
    std::pmr::vector<int> values(&resource);

    for (int i = 0; i < 256; ++i)
        values.push_back(i);

    zassert_equal(values.size(), 256);
    zassert_equal(values.back(), 255);
    zassert_true(values.get_allocator().resource() == &resource);
}

ZTEST(ext_memory_resource, test_backs_pmr_string) {
    ExtMemoryResource resource;
    std::pmr::string text("a string long enough to require heap storage", &resource);

    text.append(" plus more");

    zassert_equal(text.size(), strlen("a string long enough to require heap storage plus more"));
    zassert_true(text.get_allocator().resource() == &resource);
}
