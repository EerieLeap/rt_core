#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <zephyr/ztest.h>

#include <boost/container/pmr/vector.hpp>

#include "utilities/memory/boost_memory_resource.h"

using eerie_leap::utilities::memory::BoostExtMemoryResource;
using eerie_leap::utilities::memory::ExtMemoryResource;

namespace {

class CountingResource : public std::pmr::memory_resource {
public:
    int allocate_calls = 0;
    int deallocate_calls = 0;
    size_t last_bytes = 0;
    size_t last_align = 0;

protected:
    void* do_allocate(size_t bytes, size_t align) override {
        ++allocate_calls;
        last_bytes = bytes;
        last_align = align;
        return std::pmr::new_delete_resource()->allocate(bytes, align);
    }

    void do_deallocate(void* ptr, size_t bytes, size_t align) override {
        ++deallocate_calls;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, align);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

} // namespace

ZTEST_SUITE(boost_ext_memory_resource, NULL, NULL, NULL, NULL, NULL);

ZTEST(boost_ext_memory_resource, test_delegates_to_upstream) {
    CountingResource upstream;
    BoostExtMemoryResource resource(&upstream);

    void* block = resource.allocate(96, 16);

    zassert_not_null(block);
    zassert_equal(upstream.allocate_calls, 1);
    zassert_equal(upstream.last_bytes, 96);
    zassert_equal(upstream.last_align, 16);
    zassert_equal(upstream.deallocate_calls, 0);

    resource.deallocate(block, 96, 16);

    zassert_equal(upstream.deallocate_calls, 1);
}

ZTEST(boost_ext_memory_resource, test_returns_usable_memory) {
    ExtMemoryResource upstream;
    BoostExtMemoryResource resource(&upstream);

    auto* block = static_cast<uint8_t*>(resource.allocate(64, 8));

    zassert_not_null(block);
    memset(block, 0x5A, 64);
    zassert_equal(block[63], 0x5A);

    resource.deallocate(block, 64, 8);
}

ZTEST(boost_ext_memory_resource, test_is_equal_is_identity_based) {
    ExtMemoryResource upstream;
    BoostExtMemoryResource first(&upstream);
    BoostExtMemoryResource second(&upstream);

    zassert_true(first.is_equal(first));
    zassert_false(first.is_equal(second), "distinct wrappers must not compare equal");
    zassert_true(first == first);
    zassert_true(first != second);
}

ZTEST(boost_ext_memory_resource, test_backs_boost_pmr_vector) {
    CountingResource upstream;
    BoostExtMemoryResource resource(&upstream);

    {
        boost::container::pmr::vector<int> values(&resource);

        for (int i = 0; i < 256; ++i)
            values.push_back(i);

        zassert_equal(values.size(), 256);
        zassert_equal(values.back(), 255);
        zassert_true(upstream.allocate_calls > 0);
    }

    zassert_equal(upstream.allocate_calls, upstream.deallocate_calls);
}
