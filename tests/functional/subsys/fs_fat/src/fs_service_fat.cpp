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
