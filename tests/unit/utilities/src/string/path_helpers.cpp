#include <zephyr/ztest.h>

#include "utilities/string/path_helpers.h"

using eerie_leap::utilities::string::PathHelpers;

ZTEST_SUITE(path_helpers, NULL, NULL, NULL, NULL, NULL);

ZTEST(path_helpers, test_equal_paths_are_the_same_entry) {
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "scripts/sensors/a.lua"));
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("a.lua", "a.lua"));
}

ZTEST(path_helpers, test_ASCII_case_is_ignored) {
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "SCRIPTS/Sensors/A.LUA"));
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("Boost_Control.Lua", "boost_control.lua"));
}

ZTEST(path_helpers, test_empty_names_and_dots_name_nothing) {
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("scripts//sensors/a.lua", "scripts/sensors/a.lua"));
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("./scripts/sensors/./a.lua", "scripts/sensors/a.lua"));
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("/scripts/sensors/a.lua", "scripts/sensors/a.lua"));
    zassert_true(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua/", "scripts/sensors/a.lua"));
}

ZTEST(path_helpers, test_different_entries_differ) {
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "scripts/sensors/b.lua"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "scripts/can/a.lua"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "a.lua"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors", "scripts/sensors/a.lua"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "scripts/sensors"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("scripts/sensors/a.lua", "scripts/sensors/a.lua.bak"));
}

ZTEST(path_helpers, test_a_path_without_a_name_matches_nothing) {
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("", ""));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("", "a.lua"));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("a.lua", ""));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("/", "."));
    zassert_false(PathHelpers::IsSameEntryIgnoringCase("./", "//"));
}
