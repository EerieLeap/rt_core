#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"

using eerie_leap::utilities::memory::Mrm;

ZTEST_SUITE(memory_resource_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(memory_resource_manager, test_GetDefaultPmr_is_the_std_default) {
    zassert_not_null(Mrm::GetDefaultPmr());
    zassert_equal_ptr(Mrm::GetDefaultPmr(), std::pmr::get_default_resource());
}

ZTEST(memory_resource_manager, test_accessors_are_stable) {
    zassert_equal_ptr(Mrm::GetDefaultPmr(), Mrm::GetDefaultPmr());
    zassert_equal_ptr(Mrm::GetExtPmr(), Mrm::GetExtPmr());
    zassert_equal_ptr(Mrm::GetBoostExtPmr(), Mrm::GetBoostExtPmr());
}

ZTEST(memory_resource_manager, test_GetExtPmr_is_distinct_from_default) {
    zassert_not_null(Mrm::GetExtPmr());
    zassert_true(Mrm::GetExtPmr() != Mrm::GetDefaultPmr());
    zassert_false(Mrm::GetExtPmr()->is_equal(*Mrm::GetDefaultPmr()));
    zassert_true(Mrm::GetExtPmr()->is_equal(*Mrm::GetExtPmr()));
}

ZTEST(memory_resource_manager, test_GetExtPmr_allocates) {
    auto* resource = Mrm::GetExtPmr();

    auto* block = static_cast<uint8_t*>(resource->allocate(256, 8));

    zassert_not_null(block);
    memset(block, 0x3C, 256);
    zassert_equal(block[255], 0x3C);

    resource->deallocate(block, 256, 8);
}

ZTEST(memory_resource_manager, test_GetBoostExtPmr_allocates) {
    auto* resource = Mrm::GetBoostExtPmr();

    zassert_not_null(resource);

    auto* block = static_cast<uint8_t*>(resource->allocate(256, 8));

    zassert_not_null(block);
    memset(block, 0xC3, 256);
    zassert_equal(block[255], 0xC3);

    resource->deallocate(block, 256, 8);
}

ZTEST(memory_resource_manager, test_GetDefaultPmr_backs_pmr_containers) {
    std::pmr::vector<int> values(Mrm::GetDefaultPmr());

    for (int i = 0; i < 128; ++i)
        values.push_back(i);

    zassert_equal(values.size(), 128);
    zassert_equal(values.back(), 127);
}

ZTEST(memory_resource_manager, test_GetExtPmr_backs_pmr_containers) {
    std::pmr::string text("a string long enough to require heap storage", Mrm::GetExtPmr());

    text.append(" plus more");

    zassert_equal(text.size(), strlen("a string long enough to require heap storage plus more"));
    zassert_equal_ptr(text.get_allocator().resource(), Mrm::GetExtPmr());
}
