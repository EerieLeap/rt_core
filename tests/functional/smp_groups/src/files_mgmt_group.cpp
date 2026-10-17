#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/system_domain/smp/files_mgmt_group.h"

#include "smp_groups_test_support.h"

using namespace smp_groups_test;

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::system_domain::smp::FilesMgmtGroup;

using Command = FilesMgmtGroup::Command;
using Error = FilesMgmtGroup::Error;

namespace {

constexpr uint8_t LIST = std::to_underlying(Command::LIST);

// test_a_listing_that_cannot_be_stored_is_no_memory throws on the queue: libgcc's DWARF unwinder
// takes up to 8836 bytes of stack on the QEMU targets (qemu_riscv64).
constexpr int WORK_QUEUE_STACK_SIZE = 16384;

void AssertError(const Response& response, Error error) {
    zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::FILES));
    zassert_equal(response.error.value_or(0), std::to_underlying(error),
        "Expected error %u, got %u", std::to_underlying(error), response.error.value_or(0));
}

DirectoryEntry File(std::string name, size_t size) {
    return {.name = std::move(name), .size = size, .is_directory = false};
}

DirectoryEntry Folder(std::string name) {
    return {.name = std::move(name), .size = 0, .is_directory = true};
}

// "script_<index>_" padded with 'x' to the length.
std::string NameOfLength(size_t index, size_t length) {
    std::string name = "script_" + std::to_string(index) + "_";
    name.resize(length, 'x');

    return name;
}

struct Listed {
    std::string name;
    uint32_t size = 0;
    uint32_t type = 0;
};

class Harness {
public:
    std::shared_ptr<WorkQueueThread> work_queue;
    std::shared_ptr<FakeFsService> card = std::make_shared<FakeFsService>();
    std::unique_ptr<FilesMgmtGroup> group;
    SmpTestClient client;

    Harness() {
        work_queue = std::make_shared<WorkQueueThread>("smp_groups_files_wq", WORK_QUEUE_STACK_SIZE, 5);
        zassert_true(work_queue->Initialize());

        UseCard(card);
    }

    ~Harness() {
        group.reset();
        work_queue.reset();
    }

    // Serves the group with another SD service; nullptr for none.
    void UseCard(std::shared_ptr<const IFsService> service) {
        group.reset();
        group = std::make_unique<FilesMgmtGroup>(std::move(service), work_queue);
        zassert_true(group->Initialize());
        group->Register();
    }

    Response List(std::string_view directory, uint32_t offset = 0) {
        return client.Read(SmpGroupId::FILES, LIST, CborMap().PutText("dir", directory).Put("off", offset).Build());
    }

    // Pages through the directory as a client does.
    std::vector<Listed> ListAll(std::string_view directory, size_t& pages) {
        std::vector<Listed> listed;
        pages = 0;

        while(true) {
            auto response = List(directory, listed.size());
            zassert_true(response.IsOk());
            zassert_equal(response.Value("off"), listed.size(), "A page answers the offset asked for");
            zassert_equal(response.entries.size(), response.entry_texts.size());
            pages++;

            for(size_t i = 0; i < response.entries.size(); i++) {
                listed.push_back({
                    .name = response.entry_texts[i].at("name"),
                    .size = response.entries[i].at("size"),
                    .type = response.entries[i].at("type"),
                });
            }

            if(response.entries.empty() || listed.size() >= response.Value("total"))
                break;

            zassert_true(pages < 100, "The listing does not end");
        }

        return listed;
    }
};

} // namespace

struct smp_files_group_fixture {
    Harness* harness;
};

static void* FilesGroupSetup() {
    static smp_files_group_fixture fixture{};

    return &fixture;
}

static void FilesGroupBefore(void* data) {
    static_cast<smp_files_group_fixture*>(data)->harness = new Harness();
}

static void FilesGroupAfter(void* data) {
    auto* fixture = static_cast<smp_files_group_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;
}

ZTEST_SUITE(smp_files_group, NULL, FilesGroupSetup, FilesGroupBefore, FilesGroupAfter, NULL);

ZTEST_F(smp_files_group, test_list_reports_names_sizes_and_types) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("oil.lua", 120), Folder("lib"), File("boost_control.lua", 2048)};

    auto response = harness.List("scripts");

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 3);
    zassert_equal(response.Value("off"), 0);
    zassert_equal(response.entries.size(), 3);
    zassert_str_equal(response.entry_texts[0].at("name").c_str(), "oil.lua");
    zassert_equal(response.entries[0].at("size"), 120);
    zassert_equal(response.entries[0].at("type"), 0);
    zassert_str_equal(response.entry_texts[1].at("name").c_str(), "lib");
    zassert_equal(response.entries[1].at("size"), 0);
    zassert_equal(response.entries[1].at("type"), 1, "A directory");
    zassert_str_equal(response.entry_texts[2].at("name").c_str(), "boost_control.lua");
    zassert_equal(response.entries[2].at("size"), 2048);
    zassert_equal(response.entries[2].at("type"), 0);
}

ZTEST_F(smp_files_group, test_empty_request_lists_the_root) {
    Harness& harness = *fixture->harness;
    harness.card->directories[""] = {Folder("scripts"), Folder("logs")};

    auto without_body = harness.client.Read(SmpGroupId::FILES, LIST, {});

    zassert_true(without_body.IsOk());
    zassert_equal(without_body.Value("total"), 2);
    zassert_equal(without_body.Value("off"), 0);
    zassert_equal(without_body.entries.size(), 2);
    zassert_str_equal(without_body.entry_texts[0].at("name").c_str(), "scripts");

    auto empty_map = harness.client.Read(SmpGroupId::FILES, LIST);

    zassert_true(empty_map.IsOk());
    zassert_equal(empty_map.entries.size(), 2);
}

ZTEST_F(smp_files_group, test_a_large_directory_is_paged) {
    Harness& harness = *fixture->harness;
    std::vector<DirectoryEntry> files;
    for(size_t i = 0; i < 60; i++)
        files.push_back(File(NameOfLength(i, 40), 1000 + i));
    harness.card->directories["scripts"] = files;

    size_t pages = 0;
    const auto listed = harness.ListAll("scripts", pages);

    zassert_true(pages > 1, "60 entries do not fit into one response");
    zassert_equal(listed.size(), files.size());
    for(size_t i = 0; i < files.size(); i++) {
        zassert_str_equal(listed[i].name.c_str(), files[i].name.c_str());
        zassert_equal(listed[i].size, files[i].size);
        zassert_equal(listed[i].type, 0);
    }
}

ZTEST_F(smp_files_group, test_long_names_fit_the_response) {
    Harness& harness = *fixture->harness;
    std::vector<DirectoryEntry> files;
    for(size_t i = 0; i < 10; i++)
        files.push_back(File(NameOfLength(i, 255), i));
    harness.card->directories["scripts"] = files;

    size_t pages = 0;
    const auto listed = harness.ListAll("scripts", pages);

    zassert_true(pages > 1, "Ten 255-byte names do not fit into one response");
    zassert_equal(listed.size(), files.size(), "Every page holds at least one of the longest names");
    for(size_t i = 0; i < files.size(); i++)
        zassert_str_equal(listed[i].name.c_str(), files[i].name.c_str());
}

ZTEST_F(smp_files_group, test_offset_resumes_the_listing) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1), File("b.lua", 2), File("c.lua", 3)};

    auto response = harness.List("scripts", 1);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 3);
    zassert_equal(response.Value("off"), 1);
    zassert_equal(response.entries.size(), 2);
    zassert_str_equal(response.entry_texts[0].at("name").c_str(), "b.lua");
    zassert_str_equal(response.entry_texts[1].at("name").c_str(), "c.lua");
}

ZTEST_F(smp_files_group, test_offset_past_the_end_lists_nothing) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1), File("b.lua", 2), File("c.lua", 3)};

    auto response = harness.List("scripts", 10);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 3);
    zassert_equal(response.Value("off"), 10);
    zassert_true(response.entries.empty());
}

ZTEST_F(smp_files_group, test_an_empty_directory_has_no_entries) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {};

    auto response = harness.List("scripts");

    zassert_true(response.IsOk());
    zassert_equal(response.Value("total"), 0);
    zassert_true(response.entries.empty());
}

ZTEST_F(smp_files_group, test_without_an_sd_service_is_no_card) {
    Harness& harness = *fixture->harness;
    harness.UseCard(nullptr);

    AssertError(harness.List("scripts"), Error::NO_CARD);
    AssertError(harness.client.Read(SmpGroupId::FILES, LIST, {}), Error::NO_CARD);
}

ZTEST_F(smp_files_group, test_an_unmounted_card_is_no_card) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1)};
    harness.card->available = false;

    AssertError(harness.List("scripts"), Error::NO_CARD);
    zassert_equal(harness.card->list_calls, 0, "Nothing is read without a card");

    // Unmounted between the check and the read.
    harness.card->available = true;
    harness.card->error = -ENODEV;

    AssertError(harness.List("scripts"), Error::NO_CARD);
}

ZTEST_F(smp_files_group, test_missing_directory_is_not_found) {
    Harness& harness = *fixture->harness;
    harness.card->directories[""] = {File("oil.lua", 1)};

    AssertError(harness.List("scripts"), Error::NOT_FOUND);
}

ZTEST_F(smp_files_group, test_bad_path_is_bad_path) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1)};

    AssertError(harness.List("/scripts"), Error::BAD_PATH);
    AssertError(harness.List(".."), Error::BAD_PATH);
    AssertError(harness.List("scripts/../.."), Error::BAD_PATH);
    AssertError(harness.List("scripts\\..\\.."), Error::BAD_PATH);
    AssertError(harness.List(std::string_view("scripts\0/..", 11)), Error::BAD_PATH);
    AssertError(harness.List(std::string(FilesMgmtGroup::MAX_DIR_LENGTH + 1, 'a')), Error::BAD_PATH);

    // The longest path is read.
    AssertError(harness.List(std::string(FilesMgmtGroup::MAX_DIR_LENGTH, 'a')), Error::NOT_FOUND);

    // A file.
    harness.card->error = -ENOTDIR;
    AssertError(harness.List("scripts/a.lua"), Error::BAD_PATH);
}

ZTEST_F(smp_files_group, test_read_error_is_io_error) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1)};
    harness.card->error = -EIO;

    AssertError(harness.List("scripts"), Error::IO_ERROR);
}

ZTEST_F(smp_files_group, test_a_listing_that_cannot_be_stored_is_no_memory) {
    Harness& harness = *fixture->harness;
    harness.card->directories["scripts"] = {File("a.lua", 1)};

    harness.card->throws_bad_alloc = true;
    AssertError(harness.List("scripts"), Error::NO_MEMORY);

    harness.card->throws_bad_alloc = false;
    harness.card->error = -ENOMEM;
    AssertError(harness.List("scripts"), Error::NO_MEMORY);

    harness.card->error = 0;
    zassert_true(harness.List("scripts").IsOk(), "The group answers again after a failed read");
}

ZTEST_F(smp_files_group, test_a_malformed_request_is_invalid) {
    Harness& harness = *fixture->harness;

    auto response = harness.client.Read(SmpGroupId::FILES, LIST, CborMap().Put("dir", 7).Build());

    zassert_equal(response.Value("rc"), MGMT_ERR_EINVAL);
}

ZTEST_F(smp_files_group, test_unregistered_group_is_not_supported) {
    Harness& harness = *fixture->harness;
    harness.group->Unregister();

    auto response = harness.List("scripts");

    zassert_equal(response.Value("rc"), MGMT_ERR_ENOTSUP, "As firmware without the group answers");
}
