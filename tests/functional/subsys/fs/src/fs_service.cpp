#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <string_view>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <zephyr/devicetree.h>
#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/fs/services/fs_service.h"

#include "fs_test_support.h"

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;

using fs_test::GetFsService;
using fs_test::MakeText;
using fs_test::ReadText;
using fs_test::ResetFs;
using fs_test::WriteText;

ZTEST_SUITE(fs_service, NULL, NULL, ResetFs, ResetFs, NULL);

struct TestData {
    std::string str;
    int number;
};

void SetupFiles(FsService* fs_service, std::string& name_prefix) {
    TestData test_data = {
        .str = "Some test string test_" + name_prefix,
        .number = 1234
    };

    std::string dir_path = "test_" + name_prefix + "_dir";
    bool result = fs_service->CreateDirectory(dir_path);
    zassert_true(result);

    std::string file_path_1 = "test_" + name_prefix + "_dir/test_" + name_prefix + "_file_1.txt";
    result = fs_service->WriteFile(file_path_1, &test_data, sizeof(test_data));
    zassert_true(result);

    std::string file_path_2 = "test_" + name_prefix + "_dir/test_" + name_prefix + "_file_2.txt";
    result = fs_service->WriteFile(file_path_2, &test_data, sizeof(test_data));
    zassert_true(result);

    std::string file_path_3 = "test_" + name_prefix + "_dir/test_" + name_prefix + "_file_3.txt";
    result = fs_service->WriteFile(file_path_3, &test_data, sizeof(test_data));
    zassert_true(result);

    std::string file_path_4 = "test_" + name_prefix + "_file_4.txt";
    result = fs_service->WriteFile(file_path_4, &test_data, sizeof(test_data));
    zassert_true(result);
}

ZTEST(fs_service, test_Initialize) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->Initialize());
    zassert_true(fs_service->IsAvailable());
}

// The mount entry belongs to the devicetree fstab node, so a service can come and go freely.
ZTEST(fs_service, test_service_lifetime_does_not_affect_the_mount) {
    DtFs::InitInternalFs();

    {
        FsService local_service(DtFs::GetInternalFsMp());

        zassert_true(local_service.Initialize());
        zassert_true(local_service.Format());
        zassert_true(WriteText("test_local_service.txt", "abc"));
    }

    auto* fs_service = GetFsService();

    zassert_true(fs_service->IsAvailable());
    zassert_str_equal(ReadText("test_local_service.txt").c_str(), "abc");
}

ZTEST(fs_service, test_service_without_mount_point_is_unavailable) {
    FsService orphan_service(nullptr);

    zassert_false(orphan_service.Initialize());
    zassert_false(orphan_service.IsAvailable());
    zassert_false(orphan_service.Format());
    zassert_false(orphan_service.Exists("any.txt"));
    zassert_equal(orphan_service.ListFiles().size(), 0);
}

ZTEST(fs_service, test_absolute_paths_are_rejected) {
    auto* fs_service = GetFsService();

    zassert_false(WriteText("/test_absolute.txt", "abc"));
    zassert_false(fs_service->Exists("/intfs0/test_absolute.txt"));
    zassert_false(fs_service->CreateDirectory("/test_absolute_dir"));
    zassert_false(fs_service->DeleteFile("/test_absolute.txt"));
    zassert_false(fs_service->DeleteRecursive("/"));
    zassert_equal(fs_service->ListFiles("/").size(), 0);
}

ZTEST(fs_service, test_parent_directory_references_are_rejected) {
    auto* fs_service = GetFsService();

    zassert_false(WriteText("../test_traversal.txt", "abc"));
    zassert_false(WriteText("dir/../../test_traversal.txt", "abc"));
    zassert_false(fs_service->Exists(".."));
    zassert_false(fs_service->CreateDirectory("../test_traversal_dir"));
    zassert_false(fs_service->DeleteRecursive(".."));
}

ZTEST(fs_service, test_paths_longer_than_the_buffer_are_rejected) {
    auto* fs_service = GetFsService();

    std::string long_path(300, 'a');

    zassert_false(WriteText(long_path, "abc"));
    zassert_false(fs_service->Exists(long_path));
    zassert_equal(fs_service->ListFiles().size(), 0);
}

ZTEST(fs_service, test_OpenFile) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_OpenFile.txt", "abcdef"));

    struct fs_file_t file;
    zassert_equal(fs_service->OpenFile("test_OpenFile.txt", FS_O_READ, &file), 0);

    std::array<char, 6> buffer = {};
    zassert_equal(fs_read(&file, buffer.data(), buffer.size()), 6);
    zassert_mem_equal(buffer.data(), "abcdef", buffer.size());

    fs_close(&file);
}

ZTEST(fs_service, test_OpenFile_rejects_invalid_arguments) {
    auto* fs_service = GetFsService();

    struct fs_file_t file;

    zassert_equal(fs_service->OpenFile("test_OpenFile.txt", FS_O_READ, nullptr), -EINVAL);
    zassert_equal(fs_service->OpenFile("../test_OpenFile.txt", FS_O_READ, &file), -EINVAL);
    zassert_equal(fs_service->OpenFile("/test_OpenFile.txt", FS_O_READ, &file), -EINVAL);
    zassert_true(fs_service->OpenFile("test_OpenFile_missing.txt", FS_O_READ, &file) < 0);
}

ZTEST(fs_service, test_WriteFile) {
    auto* fs_service = GetFsService();

    TestData test_data = {
        .str = "Some test string test_WriteFile",
        .number = 1234
    };

    std::string file_path = "test_WriteFile.txt";
    bool result = fs_service->WriteFile(file_path, &test_data, sizeof(test_data));

    zassert_true(result);
}

ZTEST(fs_service, test_WriteFile_truncates_existing_file) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_WriteFile_truncates.txt";

    zassert_true(WriteText(file_path, "0123456789"));
    zassert_true(WriteText(file_path, "abc"));

    zassert_equal(fs_service->GetFileSize(file_path).value(), 3);
    zassert_str_equal(ReadText(file_path).c_str(), "abc");
}

ZTEST(fs_service, test_WriteFile_appends_to_existing_file) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_WriteFile_appends.txt";

    zassert_true(WriteText(file_path, "abc"));
    zassert_true(WriteText(file_path, "def", true));

    zassert_equal(fs_service->GetFileSize(file_path).value(), 6);
    zassert_str_equal(ReadText(file_path).c_str(), "abcdef");
}

ZTEST(fs_service, test_WriteFile_append_creates_missing_file) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_WriteFile_append_creates.txt";

    zassert_true(WriteText(file_path, "abc", true));

    zassert_true(fs_service->Exists(file_path));
    zassert_str_equal(ReadText(file_path).c_str(), "abc");
}

ZTEST(fs_service, test_WriteFile_fails_when_directory_is_missing) {
    zassert_false(WriteText("test_WriteFile_missing_dir/file.txt", "abc"));
}

ZTEST(fs_service, test_ReadFile) {
    auto* fs_service = GetFsService();

    TestData test_data = {
        .str = "Some test string test_ReadFile",
        .number = 2345
    };

    std::string file_path = "test_ReadFile.txt";
    bool result = fs_service->WriteFile(file_path, &test_data, sizeof(test_data));
    zassert_true(result);

    std::array<uint8_t, sizeof(TestData)> read_buffer = {};
    size_t read_size = 0;

    bool read_result = fs_service->ReadFile(file_path, read_buffer.data(), read_buffer.size(), read_size);
    zassert_true(read_result);

    auto test_data_read = *reinterpret_cast<TestData*>(read_buffer.data());

    zassert_str_equal(test_data_read.str.c_str(), test_data.str.c_str());
    zassert_equal(test_data_read.number, test_data.number);
}

ZTEST(fs_service, test_ReadFile_stops_at_end_of_file) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_ReadFile_stops.txt";
    zassert_true(WriteText(file_path, "0123456789"));

    std::array<char, 32> read_buffer = {};
    size_t read_size = 0;

    zassert_true(fs_service->ReadFile(file_path, read_buffer.data(), read_buffer.size(), read_size));
    zassert_equal(read_size, 10);
    zassert_mem_equal(read_buffer.data(), "0123456789", 10);
}

ZTEST(fs_service, test_ReadFile_fills_smaller_buffer) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_ReadFile_smaller_buffer.txt";
    zassert_true(WriteText(file_path, "0123456789"));

    std::array<char, 4> read_buffer = {};
    size_t read_size = 0;

    zassert_true(fs_service->ReadFile(file_path, read_buffer.data(), read_buffer.size(), read_size));
    zassert_equal(read_size, read_buffer.size());
    zassert_mem_equal(read_buffer.data(), "0123", read_buffer.size());
}

ZTEST(fs_service, test_ReadFile_fails_for_missing_file) {
    auto* fs_service = GetFsService();

    std::array<char, 8> read_buffer = {};
    size_t read_size = 0;

    zassert_false(fs_service->ReadFile("test_ReadFile_missing.txt", read_buffer.data(), read_buffer.size(), read_size));
    zassert_equal(read_size, 0);
}

ZTEST(fs_service, test_Exists) {
    auto* fs_service = GetFsService();

    TestData test_data = {
        .str = "Some test string test_Exists",
        .number = 3456
    };

    std::string file_path = "test_Exists.txt";
    bool result = fs_service->WriteFile(file_path, &test_data, sizeof(test_data));
    zassert_true(result);

    bool result_exists = fs_service->Exists(file_path);
    zassert_true(result_exists);

    std::string fake_file_path = "fake_test_Exists.txt";
    bool result_not_exists = fs_service->Exists(fake_file_path);
    zassert_false(result_not_exists);
}

ZTEST(fs_service, test_CreateDirectory) {
    auto* fs_service = GetFsService();

    std::string dir_path = "test_CreateDirectory";
    bool result = fs_service->CreateDirectory(dir_path);
    zassert_true(result);

    bool result_exists = fs_service->Exists(dir_path);
    zassert_true(result_exists);

    std::string fake_dir_path= "fake_test_CreateDirectory";
    bool result_not_exists = fs_service->Exists(fake_dir_path);
    zassert_false(result_not_exists);
}

ZTEST(fs_service, test_CreateDirectory_creates_intermediate_directories) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_CreateDirectory_nested/level_1/level_2"));

    zassert_true(fs_service->Exists("test_CreateDirectory_nested"));
    zassert_true(fs_service->Exists("test_CreateDirectory_nested/level_1"));
    zassert_true(fs_service->Exists("test_CreateDirectory_nested/level_1/level_2"));
}

ZTEST(fs_service, test_CreateDirectory_is_idempotent) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_CreateDirectory_idempotent"));
    zassert_true(fs_service->CreateDirectory("test_CreateDirectory_idempotent"));

    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service, test_ListFiles) {
    auto* fs_service = GetFsService();

    std::string name_prefix = "ListFiles";
    std::string dir_path = "test_" + name_prefix + "_dir";
    SetupFiles(fs_service, name_prefix);

    auto core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 2);

    std::unordered_set<std::string> core_files_set;
    for(auto name : core_files)
        core_files_set.insert(name);
    zassert_equal(core_files_set.count(dir_path), 1);
    zassert_equal(core_files_set.count("test_ListFiles_file_4.txt"), 1);


    auto dir_files = fs_service->ListFiles("test_" + name_prefix + "_dir");
    zassert_equal(dir_files.size(), 3);

    std::unordered_set<std::string> dir_files_set;
    for(auto name : dir_files)
        dir_files_set.insert(name);
    zassert_equal(dir_files_set.count("test_ListFiles_file_1.txt"), 1);
    zassert_equal(dir_files_set.count("test_ListFiles_file_2.txt"), 1);
    zassert_equal(dir_files_set.count("test_ListFiles_file_3.txt"), 1);
}

ZTEST(fs_service, test_ListFiles_returns_empty_for_missing_directory) {
    auto* fs_service = GetFsService();

    zassert_equal(fs_service->ListFiles("test_ListFiles_missing_dir").size(), 0);
}

namespace {

// Entries come in the file system's order, so they are looked up by name.
const DirectoryEntry& EntryNamed(const std::vector<DirectoryEntry>& entries, std::string_view name) {
    const auto entry = std::ranges::find(entries, name, &DirectoryEntry::name);
    zassert_true(entry != entries.end(), "No entry named %.*s", static_cast<int>(name.size()), name.data());

    return *entry;
}

// Five files of 1 to 5 bytes.
void SetupDirectory(const std::string& dir_path) {
    zassert_true(GetFsService()->CreateDirectory(dir_path));

    for(size_t i = 0; i < 5; i++)
        zassert_true(WriteText(dir_path + "/file_" + std::to_string(i) + ".txt", MakeText(i + 1)));
}

} // namespace

ZTEST(fs_service, test_ListDirectory_lists_files_with_sizes_and_directories) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_ListDirectory_dir/lib"));
    zassert_true(WriteText("test_ListDirectory_dir/oil.lua", MakeText(120)));
    zassert_true(WriteText("test_ListDirectory_dir/boost_control.lua", MakeText(2048)));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_dir", 0, 10, entries, count), 0);
    zassert_equal(count, 3);
    zassert_equal(entries.size(), 3);
    zassert_equal(EntryNamed(entries, "oil.lua").size, 120);
    zassert_false(EntryNamed(entries, "oil.lua").is_directory);
    zassert_equal(EntryNamed(entries, "boost_control.lua").size, 2048);
    zassert_false(EntryNamed(entries, "boost_control.lua").is_directory);
    zassert_equal(EntryNamed(entries, "lib").size, 0);
    zassert_true(EntryNamed(entries, "lib").is_directory);
}

ZTEST(fs_service, test_ListDirectory_lists_the_root) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_ListDirectory_root.txt", "abc"));
    zassert_true(fs_service->CreateDirectory("test_ListDirectory_root_dir"));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("", 0, 10, entries, count), 0);
    zassert_equal(count, 2);
    zassert_equal(EntryNamed(entries, "test_ListDirectory_root.txt").size, 3);
    zassert_true(EntryNamed(entries, "test_ListDirectory_root_dir").is_directory);
}

ZTEST(fs_service, test_ListDirectory_starts_at_first_and_counts_all) {
    auto* fs_service = GetFsService();
    SetupDirectory("test_ListDirectory_paged");

    std::vector<DirectoryEntry> all;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_paged", 0, 10, all, count), 0);
    zassert_equal(count, 5);
    zassert_equal(all.size(), 5);

    std::vector<DirectoryEntry> rest;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_paged", 2, 10, rest, count), 0);
    zassert_equal(count, 5, "The count is the directory's, not the page's");
    zassert_equal(rest.size(), 3);
    for(size_t i = 0; i < rest.size(); i++)
        zassert_str_equal(rest[i].name.c_str(), all[i + 2].name.c_str());
}

ZTEST(fs_service, test_ListDirectory_keeps_at_most_max_entries) {
    auto* fs_service = GetFsService();
    SetupDirectory("test_ListDirectory_capped");

    std::vector<DirectoryEntry> all;
    size_t count = 0;
    zassert_equal(fs_service->ListDirectory("test_ListDirectory_capped", 0, 10, all, count), 0);

    std::vector<DirectoryEntry> entries;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_capped", 1, 2, entries, count), 0);
    zassert_equal(count, 5);
    zassert_equal(entries.size(), 2);
    zassert_str_equal(entries[0].name.c_str(), all[1].name.c_str());
    zassert_str_equal(entries[1].name.c_str(), all[2].name.c_str());

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_capped", 0, 0, entries, count), 0);
    zassert_equal(count, 5, "Entries are counted when none are kept");
    zassert_true(entries.empty());

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_capped", 9, 10, entries, count), 0);
    zassert_equal(count, 5);
    zassert_true(entries.empty(), "Nothing past the end");
}

ZTEST(fs_service, test_ListDirectory_accepts_a_trailing_slash) {
    auto* fs_service = GetFsService();
    SetupDirectory("test_ListDirectory_slash");

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_slash/", 0, 10, entries, count), 0);
    zassert_equal(count, 5);
}

ZTEST(fs_service, test_ListDirectory_missing_directory_is_ENOENT) {
    auto* fs_service = GetFsService();

    std::vector<DirectoryEntry> entries{DirectoryEntry{.name = "stale"}};
    size_t count = 7;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_missing", 0, 10, entries, count), -ENOENT);
    zassert_true(entries.empty());
    zassert_equal(count, 0);
}

ZTEST(fs_service, test_ListDirectory_a_file_is_ENOTDIR) {
    auto* fs_service = GetFsService();
    zassert_true(WriteText("test_ListDirectory_file.txt", "abc"));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("test_ListDirectory_file.txt", 0, 10, entries, count), -ENOTDIR);
}

ZTEST(fs_service, test_ListDirectory_rejects_parent_references) {
    auto* fs_service = GetFsService();
    zassert_true(fs_service->CreateDirectory("test_ListDirectory_parent"));

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("..", 0, 10, entries, count), -EINVAL);
    zassert_equal(fs_service->ListDirectory("test_ListDirectory_parent/../..", 0, 10, entries, count), -EINVAL);
}

ZTEST(fs_service, test_ListDirectory_rejects_absolute_and_overlong_paths) {
    auto* fs_service = GetFsService();

    std::vector<DirectoryEntry> entries;
    size_t count = 0;

    zassert_equal(fs_service->ListDirectory("/", 0, 10, entries, count), -EINVAL);
    zassert_equal(fs_service->ListDirectory(std::string(300, 'a'), 0, 10, entries, count), -EINVAL);
}

ZTEST(fs_service, test_ListDirectory_unmounted_is_ENODEV) {
    FsService orphan_service(nullptr);

    std::vector<DirectoryEntry> entries{DirectoryEntry{.name = "stale"}};
    size_t count = 7;

    zassert_equal(orphan_service.ListDirectory("", 0, 10, entries, count), -ENODEV);
    zassert_true(entries.empty());
    zassert_equal(count, 0);
}

ZTEST(fs_service, test_Delete) {
    auto* fs_service = GetFsService();

    std::string name_prefix = "Delete";
    std::string dir_path = "test_" + name_prefix + "_dir";
    SetupFiles(fs_service, name_prefix);

    auto core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 2);

    std::unordered_set<std::string> core_files_set;
    for(auto name : core_files)
        core_files_set.insert(name);
    zassert_equal(core_files_set.count(dir_path), 1);
    zassert_equal(core_files_set.count("test_Delete_file_4.txt"), 1);

    auto dir_files = fs_service->ListFiles(dir_path);
    zassert_equal(dir_files.size(), 3);

    std::unordered_set<std::string> dir_files_set;
    for(auto name : dir_files)
        dir_files_set.insert(name);
    zassert_equal(dir_files_set.count("test_Delete_file_1.txt"), 1);
    zassert_equal(dir_files_set.count("test_Delete_file_2.txt"), 1);
    zassert_equal(dir_files_set.count("test_Delete_file_3.txt"), 1);

    fs_service->DeleteFile(dir_path + "/test_Delete_file_2.txt");
    dir_files = fs_service->ListFiles(dir_path);
    zassert_equal(dir_files.size(), 2);

    dir_files_set.clear();
    for(auto name : dir_files)
        dir_files_set.insert(name);
    zassert_equal(dir_files_set.count("test_Delete_file_1.txt"), 1);
    zassert_equal(dir_files_set.count("test_Delete_file_2.txt"), 0);
    zassert_equal(dir_files_set.count("test_Delete_file_3.txt"), 1);

    fs_service->DeleteFile("test_Delete_file_4.txt");
    core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 1);

    core_files_set.clear();
    for(auto name : core_files)
        core_files_set.insert(name);
    zassert_equal(core_files_set.count(dir_path), 1);
    zassert_equal(core_files_set.count("test_Delete_file_4.txt"), 0);
}

ZTEST(fs_service, test_DeleteFile_fails_for_missing_file) {
    auto* fs_service = GetFsService();

    zassert_false(fs_service->DeleteFile("test_DeleteFile_missing.txt"));
}

ZTEST(fs_service, test_DeleteRecursive) {
    auto* fs_service = GetFsService();

    auto core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 0);

    std::string name_prefix = "DeleteRecursive";
    std::string dir_path = "test_" + name_prefix + "_dir";
    SetupFiles(fs_service, name_prefix);

    core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 2);

    auto dir_files = fs_service->ListFiles(dir_path);
    zassert_equal(dir_files.size(), 3);

    bool result = fs_service->DeleteRecursive();
    zassert_equal(result, true);
    core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 0);
}

ZTEST(fs_service, test_DeleteRecursive_empties_subdirectory_only) {
    auto* fs_service = GetFsService();

    std::string name_prefix = "DeleteRecursiveSubdir";
    std::string dir_path = "test_" + name_prefix + "_dir";
    SetupFiles(fs_service, name_prefix);

    zassert_true(fs_service->DeleteRecursive(dir_path));

    zassert_true(fs_service->Exists(dir_path));
    zassert_equal(fs_service->ListFiles(dir_path).size(), 0);
    zassert_equal(fs_service->ListFiles().size(), 2);
}

// The failed recursive walk is reported instead of being swallowed.
ZTEST(fs_service, test_DeleteRecursive_fails_for_missing_directory) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_DeleteRecursive_kept.txt", "abc"));
    zassert_false(fs_service->DeleteRecursive("test_DeleteRecursive_missing_dir"));

    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service, test_Rename_moves_a_file_within_a_directory) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_Rename_within"));
    zassert_true(WriteText("test_Rename_within/old.txt", "abcdef"));

    zassert_equal(fs_service->Rename("test_Rename_within/old.txt", "test_Rename_within/new.txt"), 0);

    zassert_false(fs_service->Exists("test_Rename_within/old.txt"));
    zassert_str_equal(ReadText("test_Rename_within/new.txt").c_str(), "abcdef");
    zassert_equal(fs_service->ListFiles("test_Rename_within").size(), 1);
}

ZTEST(fs_service, test_Rename_moves_a_file_across_directories) {
    auto* fs_service = GetFsService();
    const std::string content = MakeText(2500);

    zassert_true(fs_service->CreateDirectory("test_Rename_from"));
    zassert_true(fs_service->CreateDirectory("test_Rename_to"));
    zassert_true(WriteText("test_Rename_from/file.txt", content));

    zassert_equal(fs_service->Rename("test_Rename_from/file.txt", "test_Rename_to/moved.txt"), 0);

    zassert_false(fs_service->Exists("test_Rename_from/file.txt"));
    zassert_equal(fs_service->ListFiles("test_Rename_from").size(), 0);
    zassert_true(ReadText("test_Rename_to/moved.txt") == content);

    zassert_equal(fs_service->Rename("test_Rename_to/moved.txt", "root.txt"), 0);

    zassert_false(fs_service->Exists("test_Rename_to/moved.txt"));
    zassert_true(ReadText("root.txt") == content);
}

ZTEST(fs_service, test_Rename_replaces_an_existing_target) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_Rename_source.txt", "the new content"));
    zassert_true(WriteText("test_Rename_target.txt", "the file that is replaced"));

    zassert_equal(fs_service->Rename("test_Rename_source.txt", "test_Rename_target.txt"), 0);

    zassert_false(fs_service->Exists("test_Rename_source.txt"));
    zassert_str_equal(ReadText("test_Rename_target.txt").c_str(), "the new content");
    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service, test_Rename_a_missing_source_fails_and_changes_nothing) {
    auto* fs_service = GetFsService();

    zassert_equal(fs_service->Rename("test_Rename_missing.txt", "test_Rename_target.txt"), -ENOENT);
    zassert_false(fs_service->Exists("test_Rename_target.txt"));
    zassert_equal(fs_service->ListFiles().size(), 0);

    // The file it would have replaced stays, though FAT deletes a target before it looks for the source.
    zassert_true(WriteText("test_Rename_kept.txt", "kept"));

    zassert_equal(fs_service->Rename("test_Rename_missing.txt", "test_Rename_kept.txt"), -ENOENT);
    zassert_str_equal(ReadText("test_Rename_kept.txt").c_str(), "kept");
    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service, test_Rename_a_missing_target_directory_fails_and_keeps_the_source) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_Rename_orphan.txt", "abc"));

    zassert_equal(fs_service->Rename("test_Rename_orphan.txt", "test_Rename_missing_dir/orphan.txt"), -ENOENT);

    zassert_str_equal(ReadText("test_Rename_orphan.txt").c_str(), "abc");
    zassert_false(fs_service->Exists("test_Rename_missing_dir"));
}

// Zephyr's FAT rename deletes the target first, so a pair of paths naming one file must not reach
// it. The service refuses such pairs on every volume, though littlefs keeps names that differ in
// case apart.
ZTEST(fs_service, test_Rename_rejects_the_same_file_in_any_spelling) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_Rename_same"));
    zassert_true(WriteText("test_Rename_same/file.txt", "abc"));

    for(const char* to : {
        "test_Rename_same/file.txt",
        "test_Rename_same/FILE.txt",
        "TEST_RENAME_SAME/File.TXT",
        "test_Rename_same//file.txt",
        "test_Rename_same/./file.txt",
        "./test_Rename_same/file.txt",
    }) {
        zassert_equal(fs_service->Rename("test_Rename_same/file.txt", to), -EINVAL, "Renamed onto %s", to);
        zassert_equal(fs_service->Rename(to, "test_Rename_same/file.txt"), -EINVAL, "Renamed from %s", to);
        zassert_str_equal(ReadText("test_Rename_same/file.txt").c_str(), "abc");
    }

    zassert_equal(fs_service->ListFiles("test_Rename_same").size(), 1);
}

// FatFs reads a backslash as a separator, drops the trailing dots and spaces of a name and ends a
// path at a control byte, so on FAT most of these name oil.lua itself. littlefs keeps them apart, but
// the service refuses every spelling outside its plain alphabet on every volume.
ZTEST(fs_service, test_Rename_rejects_names_outside_the_plain_alphabet) {
    auto* fs_service = GetFsService();

    const std::string_view oil = "test_Rename_alphabet/oil.lua";

    zassert_true(fs_service->CreateDirectory("test_Rename_alphabet"));
    zassert_true(WriteText(oil, "abc"));

    for(const char* spelling : {
        "test_Rename_alphabet\\oil.lua",
        "test_Rename_alphabet/oil.lua.",
        "test_Rename_alphabet/oil.lua..",
        "test_Rename_alphabet/oil.lua ",
        "test_Rename_alphabet/oil.lua. .",
        "test_Rename_alphabet./oil.lua",
        "test_Rename_alphabet /oil.lua",
        "test_Rename_alphabet/oil.lua\001x",
        "test_Rename_alphabet/oil v2.lua",
        "test_Rename_alphabet/oil~1.lua",
        "test_Rename_alphabet/oil.lua:x",
        "test_Rename_alphabet/oil*.lua",
        "test_Rename_alphabet/oil\303\251.lua",
    }) {
        zassert_equal(fs_service->Rename(oil, spelling), -EINVAL, "Renamed onto \"%s\"", spelling);
        zassert_equal(fs_service->Rename(spelling, oil), -EINVAL, "Renamed from \"%s\"", spelling);
        zassert_str_equal(ReadText(oil).c_str(), "abc");
    }

    zassert_equal(fs_service->ListFiles().size(), 1);
    zassert_equal(fs_service->ListFiles("test_Rename_alphabet").size(), 1);
}

// The paths the asset store renames: a staged file of a directory with a leading dot onto an asset.
ZTEST(fs_service, test_Rename_accepts_dots_dashes_and_underscores_in_names) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory(".assets_tmp"));
    zassert_true(fs_service->CreateDirectory("images"));
    zassert_true(WriteText(".assets_tmp/upload.part", "abc"));

    zassert_equal(fs_service->Rename(".assets_tmp/upload.part", "images/Logo_v2-final.1.PNG"), 0);

    zassert_false(fs_service->Exists(".assets_tmp/upload.part"));
    zassert_str_equal(ReadText("images/Logo_v2-final.1.PNG").c_str(), "abc");
}

ZTEST(fs_service, test_Rename_rejects_absolute_parent_empty_and_overlong_paths) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_Rename_invalid.txt", "abc"));

    zassert_equal(fs_service->Rename("/test_Rename_invalid.txt", "test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("test_Rename_invalid.txt", "/test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("../test_Rename_invalid.txt", "test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("test_Rename_invalid.txt", "../test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("test_Rename_invalid.txt", "dir/../../test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("", "test_Rename_other.txt"), -EINVAL);
    zassert_equal(fs_service->Rename("test_Rename_invalid.txt", ""), -EINVAL);
    zassert_equal(fs_service->Rename("test_Rename_invalid.txt", std::string(300, 'a')), -EINVAL);

    zassert_str_equal(ReadText("test_Rename_invalid.txt").c_str(), "abc");
    zassert_false(fs_service->Exists("test_Rename_other.txt"));
    zassert_equal(fs_service->ListFiles().size(), 1);
}

ZTEST(fs_service, test_Rename_unmounted_is_ENODEV) {
    FsService orphan_service(nullptr);

    zassert_equal(orphan_service.Rename("test_Rename_old.txt", "test_Rename_new.txt"), -ENODEV);
}

ZTEST(fs_service, test_GetFileSize) {
    auto* fs_service = GetFsService();

    std::string file_path = "test_GetFileSize.txt";
    auto content = MakeText(1234);

    zassert_true(WriteText(file_path, content));
    zassert_equal(fs_service->GetFileSize(file_path).value(), content.size());
}

ZTEST(fs_service, test_GetFileSize_fails_for_missing_file) {
    auto* fs_service = GetFsService();

    zassert_false(fs_service->GetFileSize("test_GetFileSize_missing.txt").has_value());
}

ZTEST(fs_service, test_GetFileSize_reports_zero_for_an_empty_file) {
    auto* fs_service = GetFsService();

    zassert_true(WriteText("test_GetFileSize_empty.txt", ""));
    zassert_equal(fs_service->GetFileSize("test_GetFileSize_empty.txt").value(), 0);
}

// littlefs leaves the size of a directory undefined, so there is nothing to report for one.
ZTEST(fs_service, test_GetFileSize_fails_for_a_directory) {
    auto* fs_service = GetFsService();

    zassert_true(fs_service->CreateDirectory("test_GetFileSize_dir/nested"));
    zassert_true(WriteText("test_GetFileSize_dir/file.txt", "abc"));

    zassert_false(fs_service->GetFileSize("test_GetFileSize_dir").has_value());
    zassert_false(fs_service->GetFileSize("test_GetFileSize_dir/nested").has_value());
    zassert_false(fs_service->GetFileSize("").has_value(), "The root of the volume is a directory too");
    zassert_equal(fs_service->GetFileSize("test_GetFileSize_dir/file.txt").value(), 3);
}

ZTEST(fs_service, test_GetTotalSpace_and_GetUsedSpace) {
    auto* fs_service = GetFsService();

    uint64_t total_space = fs_service->GetTotalSpace();
    uint64_t used_space = fs_service->GetUsedSpace();

    zassert_true(total_space > 0);
    zassert_true(used_space < total_space);

    zassert_true(WriteText("test_GetUsedSpace.bin", MakeText(64 * 1024)));

    uint64_t used_space_after_write = fs_service->GetUsedSpace();

    zassert_true(used_space_after_write > used_space);
    zassert_true(used_space_after_write <= total_space);
    zassert_equal(fs_service->GetTotalSpace(), total_space);
}

namespace {

// The volume is the 1 MiB intfs0_partition, which littlefs divides into blocks of the flash's erase
// size. They are its allocation units (f_frsize), where f_bsize is only the 16 byte prog size.
constexpr uint64_t VOLUME_SIZE = DT_REG_SIZE(DT_NODELABEL(intfs0_partition));
constexpr uint64_t ALLOCATION_UNIT = DT_PROP(DT_GPARENT(DT_NODELABEL(intfs0_partition)), erase_block_size);

static_assert(VOLUME_SIZE % ALLOCATION_UNIT == 0, "The volume is a whole number of blocks");

constexpr uint64_t RoundUp(uint64_t value, uint64_t unit) {
    return (value + unit - 1) / unit * unit;
}

} // namespace

ZTEST(fs_service, test_GetTotalSpace_is_the_size_of_the_volume) {
    const uint64_t total_space = GetFsService()->GetTotalSpace();

    zassert_equal(total_space, VOLUME_SIZE, "The service reports %llu bytes of a %llu byte volume",
        static_cast<unsigned long long>(total_space), static_cast<unsigned long long>(VOLUME_SIZE));
}

// The files are larger than littlefs keeps inline in a directory, so each takes blocks of its own.
ZTEST(fs_service, test_GetUsedSpace_grows_by_at_least_the_bytes_written_in_whole_blocks) {
    auto* fs_service = GetFsService();

    for(const size_t size : {size_t{5000}, size_t{64 * 1024}}) {
        const uint64_t used_space = fs_service->GetUsedSpace();

        zassert_true(WriteText("test_GetUsedSpace_" + std::to_string(size) + ".bin", MakeText(size)));

        const uint64_t growth = fs_service->GetUsedSpace() - used_space;

        zassert_true(growth >= RoundUp(size, ALLOCATION_UNIT), "%llu bytes took %llu bytes of space",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(growth));
    }
}

ZTEST(fs_service, test_Format) {
    auto* fs_service = GetFsService();

    auto core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 0);

    std::string name_prefix = "Format";
    std::string dir_path = "test_" + name_prefix + "_dir";
    SetupFiles(fs_service, name_prefix);

    core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 2);

    auto dir_files = fs_service->ListFiles(dir_path);
    zassert_equal(dir_files.size(), 3);

    fs_service->Format();
    core_files = fs_service->ListFiles();
    zassert_equal(core_files.size(), 0);

    zassert_true(fs_service->IsAvailable());
}
