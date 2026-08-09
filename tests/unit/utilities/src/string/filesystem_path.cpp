#include <string_view>
#include <zephyr/ztest.h>

#include "utilities/string/filesystem_path.hpp"

using eerie_leap::utilities::string::FilesystemPath;

ZTEST_SUITE(filesystem_path, NULL, NULL, NULL, NULL, NULL);

ZTEST(filesystem_path, test_default_is_empty) {
    FilesystemPath<64> path;

    zassert_true(path.String().Empty());
    zassert_str_equal(path.String().CStr(), "");
}

ZTEST(filesystem_path, test_construction_from_string_view) {
    FilesystemPath<64> path("/lfs/config.json");

    zassert_true(path.String() == "/lfs/config.json");
}

ZTEST(filesystem_path, test_String_const_accessor) {
    const FilesystemPath<64> path("/lfs");

    zassert_str_equal(path.String().CStr(), "/lfs");
    zassert_equal(path.String().Size(), 4);
}

ZTEST(filesystem_path, test_append_inserts_separator) {
    FilesystemPath<64> path("/lfs");

    path /= "config.json";

    zassert_true(path.String() == "/lfs/config.json");
}

ZTEST(filesystem_path, test_append_collapses_trailing_separator) {
    FilesystemPath<64> path("/lfs/");

    path /= "config.json";

    zassert_true(path.String() == "/lfs/config.json");
}

ZTEST(filesystem_path, test_append_collapses_repeated_trailing_separators) {
    FilesystemPath<64> path("/lfs///");

    path /= "config.json";

    zassert_true(path.String() == "/lfs/config.json");
}

ZTEST(filesystem_path, test_append_to_empty_adds_no_separator) {
    FilesystemPath<64> path;

    path /= "config.json";

    zassert_true(path.String() == "config.json");
}

ZTEST(filesystem_path, test_append_empty_is_noop) {
    FilesystemPath<64> path("/lfs");

    path /= "";

    zassert_true(path.String() == "/lfs");
}

ZTEST(filesystem_path, test_absolute_rhs_replaces_path) {
    FilesystemPath<64> path("/lfs/config");

    path /= "/etc";

    zassert_true(path.String() == "/etc");
}

ZTEST(filesystem_path, test_append_to_root) {
    FilesystemPath<64> path("/");

    path /= "lfs";

    zassert_true(path.String() == "/lfs");
}

ZTEST(filesystem_path, test_append_is_chainable) {
    FilesystemPath<64> path("/lfs");

    path /= "sensors";
    path /= "config.json";

    zassert_true(path.String() == "/lfs/sensors/config.json");
}

ZTEST(filesystem_path, test_parent_path_of_nested_path) {
    FilesystemPath<64> path("/lfs/sensors/config.json");

    zassert_true(path.parent_path().String() == "/lfs/sensors");
}

ZTEST(filesystem_path, test_parent_path_drops_trailing_separator_only) {
    FilesystemPath<64> nested("foo/bar/");
    FilesystemPath<64> single("foo/");
    FilesystemPath<64> absolute("/lfs/sensors/");

    zassert_true(nested.parent_path().String() == "foo/bar");
    zassert_true(single.parent_path().String() == "foo");
    zassert_true(absolute.parent_path().String() == "/lfs/sensors");
}

ZTEST(filesystem_path, test_parent_path_collapses_redundant_separators) {
    FilesystemPath<64> doubled("foo//bar");
    FilesystemPath<64> trailing("/lfs//");
    FilesystemPath<64> separators_only("///");

    zassert_true(doubled.parent_path().String() == "foo");
    zassert_true(trailing.parent_path().String() == "/lfs");
    zassert_true(separators_only.parent_path().String() == "/");
}

ZTEST(filesystem_path, test_parent_path_without_separator_is_empty) {
    FilesystemPath<64> path("config.json");

    zassert_true(path.parent_path().String().Empty());
}

ZTEST(filesystem_path, test_parent_path_of_root_is_root) {
    FilesystemPath<64> path("/");

    zassert_true(path.parent_path().String() == "/");
}

ZTEST(filesystem_path, test_parent_path_of_top_level_entry_is_root) {
    FilesystemPath<64> path("/lfs");

    zassert_true(path.parent_path().String() == "/");
}

ZTEST(filesystem_path, test_parent_path_of_empty_is_empty) {
    FilesystemPath<64> path;

    zassert_true(path.parent_path().String().Empty());
}

ZTEST(filesystem_path, test_parent_path_does_not_modify_source) {
    FilesystemPath<64> path("/lfs/config.json");

    auto parent = path.parent_path();

    zassert_true(parent.String() == "/lfs");
    zassert_true(path.String() == "/lfs/config.json");
}

ZTEST(filesystem_path, test_parent_path_is_repeatable) {
    FilesystemPath<64> path("/lfs/sensors/config.json");

    zassert_true(path.parent_path().parent_path().String() == "/lfs");
    zassert_true(path.parent_path().parent_path().parent_path().String() == "/");
}

ZTEST(filesystem_path, test_String_is_mutable) {
    FilesystemPath<64> path("/lfs");

    path.String().Append("/data");

    zassert_true(path.String() == "/lfs/data");
}
