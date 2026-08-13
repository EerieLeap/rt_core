#include <array>
#include <string>
#include <cstdint>
#include <unordered_set>
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
