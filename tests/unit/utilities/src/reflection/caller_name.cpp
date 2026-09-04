#include <cstdint>
#include <string_view>

#include <zephyr/ztest.h>

#include "utilities/reflection/caller_name.h"

using eerie_leap::utilities::reflection::CallerName;
using eerie_leap::utilities::reflection::GetCallerName;
using eerie_leap::utilities::string::StringHelpers;

namespace {

// static constexpr forces constant evaluation, so a regression that made GetCallerName
// runtime-only would fail to compile rather than fail a check.
CallerName FirstCaller() {
    static constexpr auto caller = GetCallerName();

    return caller;
}

CallerName SecondCaller() {
    static constexpr auto caller = GetCallerName();

    return caller;
}

} // namespace

ZTEST_SUITE(reflection_caller_name, NULL, NULL, NULL, NULL, NULL);

ZTEST(reflection_caller_name, test_GetCallerName_is_stable_across_calls) {
    zassert_equal(FirstCaller().hash, FirstCaller().hash);
}

ZTEST(reflection_caller_name, test_GetCallerName_distinguishes_functions) {
    zassert_not_equal(FirstCaller().hash, SecondCaller().hash);
}

ZTEST(reflection_caller_name, test_GetCallerName_identifies_the_enclosing_function) {
    std::string_view name = FirstCaller().name;

    // An empty name means the location was captured outside a function body.
    zassert_false(name.empty());
    zassert_true(name.find("FirstCaller") != std::string_view::npos);
}

ZTEST(reflection_caller_name, test_GetCallerName_hash_matches_its_name) {
    auto caller = FirstCaller();

    zassert_equal(caller.hash, StringHelpers::GetHash(caller.name));
}
