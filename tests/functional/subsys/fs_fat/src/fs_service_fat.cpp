#include <algorithm>
#include <cerrno>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::FsService;

namespace {

// The volume the overlay mounts at boot: FAT with long names, as an SD card is.
FsService* GetFatService() {
    static FsService* fs_service = nullptr;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        fs_service = new FsService(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Initialize(), "The FAT volume is not mounted");
    }

    return fs_service;
}

void ResetFat(void* = nullptr) {
    GetFatService()->DeleteRecursive();
}

bool WriteText(std::string_view relative_path, std::string_view content) {
    return GetFatService()->WriteFile(relative_path, content.data(), content.size());
}

std::string ReadText(std::string_view relative_path) {
    auto* fs_service = GetFatService();

    std::vector<char> buffer(fs_service->GetFileSize(relative_path).value_or(0) + 1);
    size_t read_size = 0;

    if(!fs_service->ReadFile(relative_path, buffer.data(), buffer.size(), read_size))
        return {};

    return {buffer.data(), read_size};
}

// Entries come in the file system's order, so they are looked up by name.
const DirectoryEntry& EntryNamed(const std::vector<DirectoryEntry>& entries, std::string_view name) {
    const auto entry = std::ranges::find(entries, name, &DirectoryEntry::name);
    zassert_true(entry != entries.end(), "No entry named %.*s", static_cast<int>(name.size()), name.data());

    return *entry;
}

} // namespace

ZTEST_SUITE(fs_service_fat, NULL, NULL, ResetFat, ResetFat, NULL);

ZTEST(fs_service_fat, test_ListDirectory_lists_long_names_sizes_and_directories) {
    auto* fs_service = GetFatService();
    const std::string long_name = std::string(100, 'b') + ".lua";

    zassert_true(fs_service->CreateDirectory("scripts/lib"));
    zassert_true(WriteText("scripts/oil.lua", "abc"));
    zassert_true(WriteText("scripts/Boost_Map.lua", std::string(1234, 'a')));
    zassert_true(WriteText("scripts/" + long_name, ""));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("scripts", 0, 10, entries, count), 0);
    zassert_equal(count, 4);
    zassert_equal(entries.size(), 4);
    zassert_equal(EntryNamed(entries, "oil.lua").size, 3);
    zassert_false(EntryNamed(entries, "oil.lua").is_directory);
    zassert_equal(EntryNamed(entries, "Boost_Map.lua").size, 1234, "The name keeps its case");
    zassert_equal(EntryNamed(entries, long_name).size, 0);
    zassert_true(EntryNamed(entries, "lib").is_directory);
    zassert_equal(EntryNamed(entries, "lib").size, 0);
}

// FAT stores a size of 0 for a directory, which is no file size either.
ZTEST(fs_service_fat, test_GetFileSize_fails_for_a_directory) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory("scripts/lib"));
    zassert_true(WriteText("scripts/oil.lua", "abc"));

    zassert_false(fs_service->GetFileSize("scripts").has_value());
    zassert_false(fs_service->GetFileSize("scripts/lib").has_value());
    zassert_false(fs_service->GetFileSize("").has_value(), "The root of the volume is a directory too");
    zassert_equal(fs_service->GetFileSize("scripts/oil.lua").value(), 3);
}

ZTEST(fs_service_fat, test_ListDirectory_lists_the_root) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory("scripts"));
    zassert_true(WriteText("readme.txt", "abc"));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("", 0, 10, entries, count), 0);
    zassert_equal(count, 2);
    zassert_true(EntryNamed(entries, "scripts").is_directory);
    zassert_equal(EntryNamed(entries, "readme.txt").size, 3);
}

ZTEST(fs_service_fat, test_ListDirectory_pages_and_accepts_a_trailing_slash) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory("scripts"));
    for(int i = 0; i < 5; i++)
        zassert_true(WriteText("scripts/script_" + std::to_string(i) + ".lua", "x"));

    std::vector<DirectoryEntry> all;
    size_t count = 0;
    zassert_equal(fs_service->ListDirectory("scripts/", 0, 10, all, count), 0);
    zassert_equal(count, 5);

    std::vector<DirectoryEntry> page;

    zassert_equal(fs_service->ListDirectory("scripts", 2, 2, page, count), 0);
    zassert_equal(count, 5);
    zassert_equal(page.size(), 2);
    zassert_str_equal(page[0].name.c_str(), all[2].name.c_str());
    zassert_str_equal(page[1].name.c_str(), all[3].name.c_str());
}

ZTEST(fs_service_fat, test_ListDirectory_a_file_is_ENOTDIR) {
    auto* fs_service = GetFatService();
    zassert_true(WriteText("oil.lua", "abc"));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("oil.lua", 0, 10, entries, count), -ENOTDIR,
        "FAT reports a file as a missing directory, which the service tells apart");
}

ZTEST(fs_service_fat, test_ListDirectory_missing_directory_is_ENOENT) {
    auto* fs_service = GetFatService();

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("scripts", 0, 10, entries, count), -ENOENT);
    zassert_equal(fs_service->ListDirectory("scripts/lib", 0, 10, entries, count), -ENOENT);
}

ZTEST(fs_service_fat, test_Rename_moves_a_file_within_and_across_directories) {
    auto* fs_service = GetFatService();
    const std::string content(1234, 'a');

    zassert_true(fs_service->CreateDirectory("scripts"));
    zassert_true(fs_service->CreateDirectory("archive"));
    zassert_true(WriteText("scripts/oil.lua", content));

    zassert_equal(fs_service->Rename("scripts/oil.lua", "scripts/oil_v2.lua"), 0);

    zassert_false(fs_service->Exists("scripts/oil.lua"));
    zassert_true(ReadText("scripts/oil_v2.lua") == content);

    zassert_equal(fs_service->Rename("scripts/oil_v2.lua", "archive/oil.lua"), 0);

    zassert_false(fs_service->Exists("scripts/oil_v2.lua"));
    zassert_equal(fs_service->ListFiles("scripts").size(), 0);
    zassert_true(ReadText("archive/oil.lua") == content);
}

ZTEST(fs_service_fat, test_Rename_replaces_an_existing_target) {
    auto* fs_service = GetFatService();

    zassert_true(WriteText("oil.lua", "the new script"));
    zassert_true(WriteText("boost.lua", "the script that is replaced"));

    zassert_equal(fs_service->Rename("oil.lua", "boost.lua"), 0);

    zassert_false(fs_service->Exists("oil.lua"));
    zassert_str_equal(ReadText("boost.lua").c_str(), "the new script");
    zassert_equal(fs_service->ListFiles().size(), 1);
}

// Zephyr's FAT rename deletes the target before it looks for the source.
ZTEST(fs_service_fat, test_Rename_a_missing_source_fails_and_keeps_the_target) {
    auto* fs_service = GetFatService();

    zassert_equal(fs_service->Rename("oil.lua", "oil_v2.lua"), -ENOENT);
    zassert_false(fs_service->Exists("oil_v2.lua"));
    zassert_equal(fs_service->ListFiles().size(), 0);

    zassert_true(WriteText("boost.lua", "kept"));

    zassert_equal(fs_service->Rename("oil.lua", "boost.lua"), -ENOENT);
    zassert_str_equal(ReadText("boost.lua").c_str(), "kept");
    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service_fat, test_Rename_a_missing_target_directory_fails_and_keeps_the_source) {
    auto* fs_service = GetFatService();

    zassert_true(WriteText("oil.lua", "abc"));

    zassert_equal(fs_service->Rename("oil.lua", "scripts/oil.lua"), -ENOENT);

    zassert_str_equal(ReadText("oil.lua").c_str(), "abc");
    zassert_false(fs_service->Exists("scripts"));
}

// FAT folds case, so these all name the file itself, which Zephyr's rename would delete.
ZTEST(fs_service_fat, test_Rename_rejects_the_same_file_in_any_case) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory("scripts"));
    zassert_true(WriteText("scripts/Oil.lua", "abc"));

    for(const char* to : {
        "scripts/Oil.lua",
        "scripts/oil.lua",
        "SCRIPTS/OIL.LUA",
        "scripts//Oil.lua",
        "scripts/./Oil.lua",
    }) {
        zassert_equal(fs_service->Rename("scripts/Oil.lua", to), -EINVAL, "Renamed onto %s", to);
        zassert_equal(fs_service->Rename(to, "scripts/Oil.lua"), -EINVAL, "Renamed from %s", to);
        zassert_str_equal(ReadText("scripts/Oil.lua").c_str(), "abc");
    }

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("scripts", 0, 10, entries, count), 0);
    zassert_equal(count, 1);
    zassert_equal(EntryNamed(entries, "Oil.lua").size, 3, "The name keeps its case");
}

// FatFs reads a backslash as a separator, drops the trailing dots and spaces of a name and ends a
// path at a control byte, so these name oil.lua itself, which Zephyr's rename would delete. The rest
// are outside the alphabet in which FAT and littlefs resolve a name as it is written.
ZTEST(fs_service_fat, test_Rename_rejects_names_outside_the_plain_alphabet) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory("scripts"));
    zassert_true(WriteText("scripts/oil.lua", "abc"));

    for(const char* spelling : {
        "scripts\\oil.lua",
        "scripts/oil.lua.",
        "scripts/oil.lua..",
        "scripts/oil.lua ",
        "scripts/oil.lua. .",
        "scripts./oil.lua",
        "scripts /oil.lua",
        "scripts/oil.lua\001x",
        "scripts/oil v2.lua",
        "scripts/oil~1.lua",
        "scripts/oil.lua:x",
        "scripts/oil*.lua",
        "scripts/oil\303\251.lua",
    }) {
        zassert_equal(fs_service->Rename("scripts/oil.lua", spelling), -EINVAL, "Renamed onto \"%s\"", spelling);
        zassert_equal(fs_service->Rename(spelling, "scripts/oil.lua"), -EINVAL, "Renamed from \"%s\"", spelling);
        zassert_str_equal(ReadText("scripts/oil.lua").c_str(), "abc");
    }

    zassert_equal(fs_service->ListFiles().size(), 1);
    zassert_equal(fs_service->ListFiles("scripts").size(), 1);
}

// The paths the asset store renames: a staged file of a directory with a leading dot onto an asset.
ZTEST(fs_service_fat, test_Rename_accepts_dots_dashes_and_underscores_in_names) {
    auto* fs_service = GetFatService();

    zassert_true(fs_service->CreateDirectory(".assets_tmp"));
    zassert_true(fs_service->CreateDirectory("images"));
    zassert_true(WriteText(".assets_tmp/upload.part", "abc"));

    zassert_equal(fs_service->Rename(".assets_tmp/upload.part", "images/Logo_v2-final.1.PNG"), 0);

    zassert_false(fs_service->Exists(".assets_tmp/upload.part"));
    zassert_str_equal(ReadText("images/Logo_v2-final.1.PNG").c_str(), "abc");
}

ZTEST(fs_service_fat, test_Rename_gives_the_file_the_name_it_is_asked_for) {
    auto* fs_service = GetFatService();
    const std::string long_name = std::string(100, 'b') + ".lua";

    zassert_true(WriteText("oil.lua", "abc"));

    zassert_equal(fs_service->Rename("oil.lua", "Boost_Map.lua"), 0);
    zassert_equal(fs_service->Rename("Boost_Map.lua", long_name), 0);

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("", 0, 10, entries, count), 0);
    zassert_equal(count, 1);
    zassert_equal(EntryNamed(entries, long_name).size, 3);
    zassert_str_equal(ReadText(long_name).c_str(), "abc");
}

ZTEST(fs_service_fat, test_Rename_rejects_absolute_parent_empty_and_overlong_paths) {
    auto* fs_service = GetFatService();

    zassert_true(WriteText("oil.lua", "abc"));

    zassert_equal(fs_service->Rename("/oil.lua", "boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("oil.lua", "/boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("../oil.lua", "boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("oil.lua", "../boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("oil.lua", "scripts/../../boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("", "boost.lua"), -EINVAL);
    zassert_equal(fs_service->Rename("oil.lua", ""), -EINVAL);
    zassert_equal(fs_service->Rename("oil.lua", std::string(300, 'a')), -EINVAL);

    zassert_str_equal(ReadText("oil.lua").c_str(), "abc");
    zassert_false(fs_service->Exists("boost.lua"));
    zassert_equal(fs_service->ListFiles().size(), 1);
}
