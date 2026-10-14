#include <cstdint>
#include <limits>

#include <zephyr/ztest.h>

#include "subsys/event_bus/acceptance_cache.h"

using eerie_leap::subsys::event_bus::AcceptanceCache;

ZTEST_SUITE(event_bus_acceptance_cache, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_acceptance_cache, test_an_empty_cache_knows_no_key) {
    AcceptanceCache<4> cache;

    zassert_false(cache.Find(0).has_value());
    zassert_false(cache.Find(7).has_value());
    zassert_equal(cache.GetCount(), 0);
}

ZTEST(event_bus_acceptance_cache, test_a_stored_answer_is_found_until_the_generation_moves) {
    AcceptanceCache<4> cache;
    cache.Revalidate(0);

    cache.Store(10, true);
    cache.Store(20, false);

    zassert_true(cache.Find(10).value());
    zassert_false(cache.Find(20).value());
    zassert_equal(cache.GetCount(), 2);

    cache.Revalidate(1);

    zassert_false(cache.Find(10).has_value());
    zassert_false(cache.Find(20).has_value());
    zassert_equal(cache.GetCount(), 0);
}

ZTEST(event_bus_acceptance_cache, test_the_same_generation_keeps_the_answers) {
    AcceptanceCache<4> cache;
    cache.Revalidate(5);

    cache.Store(10, true);
    cache.Revalidate(5);
    cache.Revalidate(5);

    zassert_true(cache.Find(10).value());
    zassert_equal(cache.GetCount(), 1);
}

ZTEST(event_bus_acceptance_cache, test_a_full_cache_forgets_every_answer_before_storing) {
    AcceptanceCache<2> cache;

    cache.Store(1, true);
    cache.Store(2, false);
    zassert_equal(cache.GetCount(), 2);

    cache.Store(3, true);

    zassert_false(cache.Find(1).has_value());
    zassert_false(cache.Find(2).has_value());
    zassert_true(cache.Find(3).value());
    zassert_equal(cache.GetCount(), 1);
}

ZTEST(event_bus_acceptance_cache, test_storing_a_known_key_replaces_its_answer) {
    AcceptanceCache<2> cache;

    cache.Store(1, true);
    cache.Store(2, true);
    cache.Store(1, false);

    // A full cache still takes a known key without forgetting the others.
    zassert_equal(cache.GetCount(), 2);
    zassert_false(cache.Find(1).value());
    zassert_true(cache.Find(2).value());
}

ZTEST(event_bus_acceptance_cache, test_keys_are_found_whatever_order_they_are_stored_in) {
    AcceptanceCache<4> cache;
    constexpr uint32_t k_max = std::numeric_limits<uint32_t>::max();

    cache.Store(7, true);
    cache.Store(0, false);
    cache.Store(k_max, true);
    cache.Store(3, false);

    zassert_equal(cache.GetCount(), 4);
    zassert_true(cache.Find(7).value());
    zassert_false(cache.Find(0).value());
    zassert_true(cache.Find(k_max).value());
    zassert_false(cache.Find(3).value());
    zassert_false(cache.Find(5).has_value());
    zassert_false(cache.Find(k_max - 1).has_value());
}
