#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"
#include "subsys/assets/smp/assets_mgmt_group.h"
#include "subsys/threading/work_queue_thread.h"

#include "assets_test_support.h"
#include "smp_groups_test_support.h"

using namespace smp_groups_test;
using namespace assets_group_test;

using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetReader;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::AssetWriter;
using eerie_leap::subsys::assets::ToMask;
using eerie_leap::subsys::assets::smp::AssetsMgmtGroup;
using eerie_leap::subsys::threading::WorkQueueThread;

using Command = AssetsMgmtGroup::Command;
using Error = AssetsMgmtGroup::Error;

// ======== What the group does to its files ========

// The suite links with --wrap=fs_read,--wrap=fs_close,--wrap=fs_write, so that a test can tell what the
// group read from its files, when it closed one, and make a write fail.
namespace {

atomic_t bytes_read_from_files;
atomic_t files_closed;

// What fs_write() does: 0 is its work, a negative errno is its answer, and this writes half of it.
constexpr atomic_val_t WRITE_FAULT_SHORT = 1;
atomic_t write_fault;

} // namespace

extern "C" {

ssize_t __real_fs_read(struct fs_file_t* zfp, void* ptr, size_t size);
int __real_fs_close(struct fs_file_t* zfp);
ssize_t __real_fs_write(struct fs_file_t* zfp, const void* ptr, size_t size);

ssize_t __wrap_fs_read(struct fs_file_t* zfp, void* ptr, size_t size) {
    const ssize_t rc = __real_fs_read(zfp, ptr, size);

    if(rc > 0)
        atomic_add(&bytes_read_from_files, static_cast<atomic_val_t>(rc));

    return rc;
}

int __wrap_fs_close(struct fs_file_t* zfp) {
    atomic_inc(&files_closed);

    return __real_fs_close(zfp);
}

ssize_t __wrap_fs_write(struct fs_file_t* zfp, const void* ptr, size_t size) {
    const atomic_val_t fault = atomic_get(&write_fault);

    // A write that fails writes nothing, and one that falls short is like FAT's on a full volume.
    if(fault == WRITE_FAULT_SHORT && size > 1)
        return static_cast<ssize_t>(size / 2);

    if(fault < 0)
        return static_cast<ssize_t>(fault);

    return __real_fs_write(zfp, ptr, size);
}

} // extern "C"

namespace {

constexpr uint32_t IMAGES = std::to_underlying(AssetKind::IMAGES);
constexpr uint32_t ANIMATIONS = std::to_underlying(AssetKind::ANIMATIONS);
constexpr uint32_t SENSOR_SCRIPTS = std::to_underlying(AssetKind::SENSOR_SCRIPTS);
constexpr uint32_t CAN_SCRIPTS = std::to_underlying(AssetKind::CAN_SCRIPTS);
constexpr uint32_t LOGS = std::to_underlying(AssetKind::LOGS);

constexpr uint8_t READ_ONLY = ToMask(AssetAccess::READ);
constexpr uint8_t WRITE_ONLY = ToMask(AssetAccess::WRITE);
constexpr uint8_t ALL_ACCESS = ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE) | ToMask(AssetAccess::DELETE);

constexpr uint32_t IMAGES_MAX_SIZE = 3000;
constexpr uint32_t ANIMATIONS_MAX_SIZE = 4000;
constexpr uint32_t SCRIPTS_MAX_SIZE = 2000;

constexpr std::array<std::string_view, 2> IMAGE_EXTENSIONS = { ".png", ".bin" };
constexpr std::array<std::string_view, 1> ANIMATION_EXTENSIONS = { ".json" };
constexpr std::array<std::string_view, 1> SCRIPT_EXTENSIONS = { ".lua" };
constexpr std::array<std::string_view, 1> LOG_EXTENSIONS = { ".mdf" };
// The kind on the root of the volume lists a staged file too, if one were there under its own name.
constexpr std::array<std::string_view, 2> ROOT_EXTENSIONS = { ".lua", ".part" };

// Unwinding an exception on qemu_riscv64 needs more stack than the usual work queue has.
constexpr int WORK_QUEUE_STACK_SIZE = 16384;
// A chunk of an upload: eight of them fill the buffer of the staged file (AssetWriter::BUFFER_SIZE),
// and a request holds one with its keys.
constexpr size_t UPLOAD_CHUNK_SIZE = 512;
static_assert(8 * UPLOAD_CHUNK_SIZE == AssetWriter::BUFFER_SIZE);
static_assert(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE >= UPLOAD_CHUNK_SIZE + 256);
constexpr std::string_view STAGED_PATH = ".assets_tmp/upload.part";
// Several times the 4096 bytes of a reader's buffer, for the tests that count what is read.
constexpr size_t LARGE_FILE_SIZE = 20000;
constexpr int SESSION_TIMEOUT_MS = CONFIG_EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS;
// The idle task closes a session SESSION_TIMEOUT_MS after its last request; this leaves it twice as
// long again.
constexpr int PAST_TIMEOUT_MS = 3 * SESSION_TIMEOUT_MS;

uint32_t Crc(const Bytes& data) {
    return crc32_ieee(data.data(), data.size());
}

void AssertError(const Response& response, Error error) {
    zassert_equal(response.error_group.value_or(0), std::to_underlying(SmpGroupId::ASSETS));
    zassert_equal(
        response.error.value_or(0), std::to_underlying(error), "Expected error %u, got %u", std::to_underlying(error),
        response.error.value_or(0)
    );
}

// "img_<index>_xxx.bin" of the length, as long as the name of an asset can be.
std::string NameOfLength(size_t index, size_t length) {
    std::string name = "img_" + std::to_string(index) + "_";
    name.resize(length - 4, 'x');

    return name + ".bin";
}

// What the provider of the scripts kind is asked, and which names it says are in use.
struct InUse {
    std::set<std::string, std::less<>> names;
    std::vector<std::pair<AssetAccess, std::string>> calls;
    // The answer is an exception, as when the check itself runs out of memory.
    bool throws_bad_alloc = false;
};

struct Download {
    Bytes data;
    uint32_t crc = 0;
    size_t chunk_size = 0;
    size_t chunks = 0;
};

class Harness {
public:
    std::shared_ptr<WorkQueueThread> work_queue;
    std::shared_ptr<FsService> volume = GetSimFsService();
    // The images are on a volume that can go away, the animations on one that is not mounted.
    std::shared_ptr<SwitchableFsService> images_volume;
    std::shared_ptr<SwitchableFsService> animations_volume;
    std::shared_ptr<AssetStore> images_store;
    std::shared_ptr<AssetsManager> registry = std::make_shared<AssetsManager>();
    InUse in_use;
    std::unique_ptr<AssetsMgmtGroup> group;
    SmpTestClient client;

    Harness() {
        // What an earlier test left: a write that is made to fail, and files.
        atomic_set(&write_fault, 0);
        volume->DeleteRecursive();

        images_volume = std::make_shared<SwitchableFsService>(volume);
        animations_volume = std::make_shared<SwitchableFsService>(volume, false);

        work_queue = std::make_shared<WorkQueueThread>("smp_groups_assets_wq", WORK_QUEUE_STACK_SIZE, 5);
        zassert_true(work_queue->Initialize());

        images_store = std::make_shared<AssetStore>(images_volume, "images");
        Register(AssetKind::IMAGES, "images", images_store, IMAGE_EXTENSIONS, ALL_ACCESS, IMAGES_MAX_SIZE);
        Register(
            AssetKind::ANIMATIONS, "animations", std::make_shared<AssetStore>(animations_volume, "animations"),
            ANIMATION_EXTENSIONS, ALL_ACCESS, ANIMATIONS_MAX_SIZE
        );
        Register(
            AssetKind::SENSOR_SCRIPTS, "sensor_scripts", std::make_shared<AssetStore>(volume, "scripts"),
            SCRIPT_EXTENSIONS, ALL_ACCESS, SCRIPTS_MAX_SIZE, [this](AssetAccess operation, std::string_view name) {
                in_use.calls.emplace_back(operation, std::string(name));

                if(in_use.throws_bad_alloc)
                    throw std::bad_alloc();

                return in_use.names.contains(name);
            }
        );
        Register(
            AssetKind::CAN_SCRIPTS, "can_scripts", std::make_shared<AssetStore>(volume, "can_scripts"),
            SCRIPT_EXTENSIONS, WRITE_ONLY, SCRIPTS_MAX_SIZE
        );
        Register(AssetKind::LOGS, "logs", std::make_shared<AssetStore>(volume, "logs"), LOG_EXTENSIONS, READ_ONLY, 0);

        StartGroup();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() {
        // A test that failed on the way must not leave the writes after it failing.
        atomic_set(&write_fault, 0);
        group.reset();
        work_queue.reset();
        volume->DeleteRecursive();
    }

    // Registers the images kind anew on its store, as a unit does when the feature that owns it restarts.
    void ReregisterImages(uint32_t max_size = IMAGES_MAX_SIZE) {
        registry->Unregister(AssetKind::IMAGES);
        Register(AssetKind::IMAGES, "images", images_store, IMAGE_EXTENSIONS, ALL_ACCESS, max_size);
    }

    // Registers the can scripts kind anew on the root of the volume, where the directory of the staged
    // files is too.
    void ReregisterCanScriptsAtRoot() {
        registry->Unregister(AssetKind::CAN_SCRIPTS);
        Register(
            AssetKind::CAN_SCRIPTS, "can_scripts", std::make_shared<AssetStore>(volume, ""), ROOT_EXTENSIONS,
            ALL_ACCESS, SCRIPTS_MAX_SIZE
        );
    }

    // Serves the group on a new instance, as after a restart of the unit.
    void StartGroup() {
        group.reset();
        group = std::make_unique<AssetsMgmtGroup>(registry, work_queue);
        zassert_true(group->Initialize());
        group->Register();
    }

    // Writes a file, in a directory that is created when it is missing (a kind on a volume that was not
    // mounted when its store was made has none).
    void Put(std::string_view path, const Bytes& data) {
        const size_t slash = path.rfind('/');
        if(slash != std::string_view::npos && !volume->Exists(path.substr(0, slash)))
            zassert_true(volume->CreateDirectory(path.substr(0, slash)));

        zassert_true(
            volume->WriteFile(path, data.data(), data.size()), "Failed to write %s", std::string(path).c_str()
        );
    }

    std::vector<DirectoryEntry> RawEntries(std::string_view directory) {
        std::vector<DirectoryEntry> entries;
        size_t count = 0;

        zassert_equal(volume->ListDirectory(directory, 0, 100000, entries, count), 0);
        zassert_equal(entries.size(), count);

        return entries;
    }

    Response Kinds() {
        return client.Read(SmpGroupId::ASSETS, std::to_underlying(Command::KINDS));
    }

    Response List(uint32_t kind, std::optional<uint32_t> offset = std::nullopt) {
        CborMap body;
        body.Put("kind", kind);
        if(offset.has_value())
            body.Put("off", *offset);

        return client.Read(SmpGroupId::ASSETS, std::to_underlying(Command::LIST), body.Build());
    }

    Response Read(uint32_t kind, std::string_view name, uint32_t offset) {
        return client.Read(
            SmpGroupId::ASSETS, std::to_underlying(Command::READ),
            CborMap().Put("kind", kind).PutText("name", name).Put("off", offset).Build()
        );
    }

    Response Delete(uint32_t kind, std::string_view name) {
        return client.Write(
            SmpGroupId::ASSETS, std::to_underlying(Command::DELETE),
            CborMap().Put("kind", kind).PutText("name", name).Build()
        );
    }

    Response WriteFirst(
        uint32_t kind,
        std::string_view name,
        std::span<const uint8_t> chunk,
        uint32_t length,
        uint32_t crc,
        std::optional<uint32_t> token = std::nullopt
    ) {

        CborMap body;
        body.Put("kind", kind)
            .PutText("name", name)
            .Put("off", 0)
            .Put("len", length)
            .Put("crc", crc)
            .Put("data", chunk);
        if(token.has_value())
            body.Put("tok", *token);

        return client.Write(SmpGroupId::ASSETS, std::to_underlying(Command::WRITE), body.Build());
    }

    Response WriteNext(
        uint32_t kind,
        std::string_view name,
        uint32_t offset,
        std::optional<uint32_t> token,
        std::span<const uint8_t> chunk
    ) {

        CborMap body;
        body.Put("kind", kind).PutText("name", name).Put("off", offset).Put("data", chunk);
        if(token.has_value())
            body.Put("tok", *token);

        return client.Write(SmpGroupId::ASSETS, std::to_underlying(Command::WRITE), body.Build());
    }

    // Sends the chunks of @p data from @p offset on, as a client does, and checks that each answer says
    // where the next chunk goes and carries no token. Returns the answer to the last chunk, or the first
    // that is an error.
    Response SendRest(
        uint32_t kind,
        std::string_view name,
        const Bytes& data,
        uint32_t token,
        size_t offset,
        const std::function<void()>& before_last = {}
    ) {

        Response response;

        while(offset < data.size()) {
            const size_t size = std::min(UPLOAD_CHUNK_SIZE, data.size() - offset);

            if(offset + size == data.size() && before_last)
                before_last();

            response = WriteNext(kind, name, offset, token, std::span<const uint8_t>(data).subspan(offset, size));
            if(!response.IsOk())
                return response;

            offset += size;
            zassert_equal(response.Value("off"), offset);
            zassert_false(response.Has("tok"), "The token answers the first chunk only");
        }

        return response;
    }

    // Sends a file in chunks. Returns the answer to its last chunk, or the first that is an error.
    // @p crc is what the first chunk declares, the file's own unless given.
    Response UploadAll(
        uint32_t kind,
        std::string_view name,
        const Bytes& data,
        const std::function<void()>& before_last = {},
        std::optional<uint32_t> crc = std::nullopt
    ) {

        const size_t first = std::min(data.size(), UPLOAD_CHUNK_SIZE);
        auto response = WriteFirst(
            kind, name, std::span<const uint8_t>(data).first(first), data.size(), crc.value_or(Crc(data))
        );

        if(!response.IsOk() || first == data.size())
            return response;

        zassert_true(response.Has("tok"), "The first answer carries the token");
        zassert_equal(response.Value("off"), first);

        return SendRest(kind, name, data, response.Value("tok"), first, before_last);
    }

    Bytes FileContent(std::string_view path) {
        const auto size = volume->GetFileSize(path);
        zassert_true(size.has_value(), "No file %s", std::string(path).c_str());

        // One byte more than the file holds, as the asset store reads a whole file.
        Bytes data(*size + 1);
        size_t length = 0;
        zassert_true(volume->ReadFile(path, data.data(), data.size(), length));
        data.resize(length);

        return data;
    }

    bool IsStaged() {
        return volume->Exists(STAGED_PATH);
    }

    // The chunk a READ answers with.
    static const Bytes& DataOf(const Response& response) {
        zassert_true(response.IsOk(), "The read failed with %u", response.error.value_or(0));
        zassert_true(response.bytes.contains("data"));

        return response.bytes.at("data");
    }

    // Reads a file of @p length bytes from @p offset to its end, as a client does, and checks what
    // the protocol promises of every response: the offset, `len` in the first chunk only, `crc` in
    // the last only, and the same size for every chunk but the last.
    Download DownloadFrom(uint32_t kind, std::string_view name, uint32_t offset, uint32_t length) {
        Download download;

        while(true) {
            const auto response = Read(kind, name, offset);
            const Bytes& chunk = DataOf(response);

            zassert_equal(response.Value("off"), offset);

            if(offset == 0)
                zassert_equal(response.Value("len"), length);
            else
                zassert_false(response.Has("len"), "`len` answers the first chunk only");

            if(download.chunks == 0)
                download.chunk_size = chunk.size();

            download.chunks++;
            download.data.insert(download.data.end(), chunk.begin(), chunk.end());
            offset += chunk.size();

            zassert_true(offset <= length, "A chunk does not reach past the end of the file");

            if(offset == length) {
                zassert_true(response.Has("crc"), "The last chunk carries the CRC");
                zassert_true(chunk.size() <= download.chunk_size);
                download.crc = response.Value("crc");

                return download;
            }

            // Every chunk but the last has the size of the first one, wherever the download starts.
            zassert_false(response.Has("crc"), "Only the last chunk carries the CRC");
            zassert_true(!chunk.empty(), "A chunk before the end holds data");
            zassert_equal(chunk.size(), download.chunk_size);
            zassert_true(download.chunks < 1000, "The download does not end");
        }
    }

    Download DownloadAll(uint32_t kind, std::string_view name, uint32_t length) {
        return DownloadFrom(kind, name, 0, length);
    }

    // Pages through a kind's files as a client does, until `next` is gone.
    struct Listing {
        std::vector<std::pair<std::string, uint32_t>> files;
        size_t pages = 0;
    };

    Listing ListAll(uint32_t kind) {
        Listing listing;
        std::optional<uint32_t> offset;

        while(true) {
            const auto response = List(kind, offset);
            zassert_true(response.IsOk(), "The list failed with %u", response.error.value_or(0));
            zassert_equal(response.list_sizes.at("entries"), response.entries.size());
            listing.pages++;

            for(size_t i = 0; i < response.entries.size(); i++)
                listing.files.emplace_back(response.entry_texts[i].at("name"), response.entries[i].at("size"));

            if(!response.Has("next"))
                return listing;

            zassert_true(!offset.has_value() || response.Value("next") > *offset, "`next` moves on");
            zassert_true(listing.pages < 100, "The listing does not end");
            offset = response.Value("next");
        }
    }

private:
    void Register(
        AssetKind kind,
        std::string_view name,
        std::shared_ptr<AssetStore> store,
        std::span<const std::string_view> extensions,
        uint8_t access,
        uint32_t max_size,
        std::function<bool(AssetAccess, std::string_view)> is_in_use = {}
    ) {

        zassert_true(registry->Register(
            {
                .kind = kind,
                .name = name,
                .store = std::move(store),
                .extensions = extensions,
                .access = access,
                .max_size = max_size,
                .is_in_use = std::move(is_in_use),
            }
        ));
    }
};

} // namespace

struct smp_assets_group_fixture {
    Harness* harness;
};

static void* AssetsGroupSetup() {
    static smp_assets_group_fixture fixture{};

    return &fixture;
}

static void AssetsGroupBefore(void* data) {
    static_cast<smp_assets_group_fixture*>(data)->harness = new Harness();
}

static void AssetsGroupAfter(void* data) {
    auto* fixture = static_cast<smp_assets_group_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;
}

ZTEST_SUITE(smp_assets_group, NULL, AssetsGroupSetup, AssetsGroupBefore, AssetsGroupAfter, NULL);

// ======== KINDS ========

ZTEST_F(smp_assets_group, test_kinds_lists_the_registered_kinds_in_order) {
    Harness& harness = *fixture->harness;

    const auto response = harness.Kinds();

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 5);
    zassert_equal(response.entry_texts.size(), 5);
    zassert_equal(response.entry_flags.size(), 5);
    zassert_equal(response.entry_lists.size(), 5);

    struct Expected {
        uint32_t kind;
        const char* name;
        const char* dir;
        uint32_t access;
        uint32_t max;
        std::vector<std::string> extensions;
        bool available;
    };

    const std::array<Expected, 5> expected = { {
        { IMAGES, "images", "images", ALL_ACCESS, IMAGES_MAX_SIZE, { ".png", ".bin" }, true },
        { ANIMATIONS, "animations", "animations", ALL_ACCESS, ANIMATIONS_MAX_SIZE, { ".json" }, false },
        { SENSOR_SCRIPTS, "sensor_scripts", "scripts", ALL_ACCESS, SCRIPTS_MAX_SIZE, { ".lua" }, true },
        { CAN_SCRIPTS, "can_scripts", "can_scripts", WRITE_ONLY, SCRIPTS_MAX_SIZE, { ".lua" }, true },
        { LOGS, "logs", "logs", READ_ONLY, 0, { ".mdf" }, true },
    } };

    for(size_t i = 0; i < expected.size(); i++) {
        zassert_equal(response.entries[i].at("kind"), expected[i].kind, "Kind %u", static_cast<unsigned>(i));
        zassert_str_equal(response.entry_texts[i].at("name").c_str(), expected[i].name);
        zassert_str_equal(response.entry_texts[i].at("dir").c_str(), expected[i].dir);
        zassert_equal(response.entries[i].at("access"), expected[i].access);
        zassert_equal(response.entries[i].at("max"), expected[i].max);
        zassert_true(
            response.entry_lists[i].at("ext") == expected[i].extensions, "Extensions of kind %u",
            static_cast<unsigned>(i)
        );
        zassert_equal(response.entry_flags[i].at("avail"), expected[i].available, "Kind %u", static_cast<unsigned>(i));
    }
}

ZTEST_F(smp_assets_group, test_kinds_reports_whether_the_volume_is_mounted_now) {
    Harness& harness = *fixture->harness;

    harness.animations_volume->available = true;
    harness.images_volume->available = false;

    const auto response = harness.Kinds();

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 5);
    zassert_false(response.entry_flags[0].at("avail"), "The images went away");
    zassert_true(response.entry_flags[1].at("avail"), "The animations came");
}

ZTEST_F(smp_assets_group, test_kinds_reports_free_and_used_space_only_while_mounted) {
    Harness& harness = *fixture->harness;

    harness.images_volume->available = false;

    const auto response = harness.Kinds();

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 5);

    zassert_false(response.entry_flags[0].at("avail"));
    zassert_false(response.entries[0].contains("free"), "No space for a kind that is not mounted");
    zassert_false(response.entries[0].contains("used"));

    for(size_t i = 1; i < response.entries.size(); i++) {
        if(!response.entry_flags[i].at("avail"))
            continue;

        zassert_true(response.entries[i].contains("free"), "Kind %u", static_cast<unsigned>(i));
        zassert_true(response.entries[i].contains("used"), "Kind %u", static_cast<unsigned>(i));
        zassert_true(response.entries[i].at("free") > 0);
    }

    harness.images_volume->available = true;

    const auto mounted = harness.Kinds();

    zassert_true(mounted.IsOk());
    zassert_true(mounted.entry_flags[0].at("avail"));
    zassert_true(mounted.entries[0].contains("free"));
    zassert_true(mounted.entries[0].contains("used"));
    zassert_equal(
        mounted.entries[0].at("free"), static_cast<uint32_t>(harness.images_store->GetFreeSpace()),
        "free is the number WRITE checks"
    );
}

ZTEST_F(smp_assets_group, test_kinds_follows_the_registry) {
    Harness& harness = *fixture->harness;

    harness.registry->Unregister(AssetKind::ANIMATIONS);
    harness.registry->Unregister(AssetKind::SENSOR_SCRIPTS);

    auto response = harness.Kinds();

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 3);
    zassert_equal(response.entries[0].at("kind"), IMAGES);
    zassert_equal(response.entries[1].at("kind"), CAN_SCRIPTS);
    zassert_equal(response.entries[2].at("kind"), LOGS);
}

ZTEST_F(smp_assets_group, test_kinds_of_an_empty_registry_is_an_empty_list) {
    Harness& harness = *fixture->harness;

    for(uint8_t kind = 0; kind < eerie_leap::subsys::assets::ASSET_KIND_COUNT; kind++)
        harness.registry->Unregister(static_cast<AssetKind>(kind));

    const auto response = harness.Kinds();

    zassert_true(response.IsOk());
    zassert_true(response.entries.empty());
    zassert_true(response.list_sizes.contains("kinds"), "The list is there, empty");
    zassert_equal(response.list_sizes.at("kinds"), 0);
}

// ======== LIST ========

ZTEST_F(smp_assets_group, test_list_returns_only_files_with_valid_names) {
    Harness& harness = *fixture->harness;

    harness.Put("images/a.png", Pattern(10, 1));
    harness.Put("images/b.bin", Pattern(0, 1));
    harness.Put("images/UPPER.PNG", Pattern(5, 1));
    harness.Put("images/c.txt", Pattern(3, 1));
    harness.Put("images/.hidden.png", Pattern(3, 1));
    harness.Put("images/png", Pattern(3, 1));
    harness.Put("images/.png", Pattern(3, 1));
    harness.Put("images/with space.png", Pattern(3, 1));
    harness.Put("images/" + std::string(CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH, 'n') + ".png", Pattern(3, 1));
    zassert_true(harness.volume->CreateDirectory("images/folder.png"));
    zassert_true(harness.volume->CreateDirectory("images/folder"));

    const auto listing = harness.ListAll(IMAGES);

    zassert_equal(listing.pages, 1);

    const std::set<std::pair<std::string, uint32_t>> listed(listing.files.begin(), listing.files.end());
    const std::set<std::pair<std::string, uint32_t>> expected = { { "a.png", 10 }, { "b.bin", 0 }, { "UPPER.PNG", 5 } };

    zassert_equal(listing.files.size(), expected.size(), "No entry twice");
    zassert_true(listed == expected, "Only the files whose names are valid for the kind are listed");
}

ZTEST_F(smp_assets_group, test_list_is_per_kind) {
    Harness& harness = *fixture->harness;

    harness.Put("images/a.png", Pattern(10, 1));
    harness.Put("scripts/oil.lua", Pattern(20, 1));
    harness.Put("logs/run.mdf", Pattern(30, 1));

    const auto scripts = harness.ListAll(SENSOR_SCRIPTS);
    zassert_equal(scripts.files.size(), 1);
    zassert_str_equal(scripts.files[0].first.c_str(), "oil.lua");
    zassert_equal(scripts.files[0].second, 20);

    const auto logs = harness.ListAll(LOGS);
    zassert_equal(logs.files.size(), 1);
    zassert_str_equal(logs.files[0].first.c_str(), "run.mdf");

    zassert_true(harness.ListAll(CAN_SCRIPTS).files.empty(), "A kind whose directory is empty lists nothing");
}

ZTEST_F(smp_assets_group, test_list_pages_through_entries_that_are_no_assets) {
    Harness& harness = *fixture->harness;
    constexpr size_t PADDING = 60;
    constexpr size_t ASSETS = 12;

    // The names sort before and after the assets, so that the first page holds no asset at all.
    for(size_t i = 0; i < PADDING; i++)
        harness.Put("images/a" + std::to_string(1000 + i) + ".txt", Pattern(1, 1));
    for(size_t i = 0; i < ASSETS; i++)
        harness.Put("images/b" + std::to_string(1000 + i) + ".bin", Pattern(i, 1));
    for(size_t i = 0; i < PADDING; i++)
        harness.Put("images/c" + std::to_string(1000 + i) + ".txt", Pattern(1, 1));

    // A directory lists in the file system's own order, which the pages must follow.
    const auto raw = harness.RawEntries("images");
    constexpr size_t PAGE = AssetsMgmtGroup::MAX_PAGE_ENTRIES;

    zassert_equal(raw.size(), 2 * PADDING + ASSETS);
    zassert_true(raw.size() > 2 * PAGE, "More raw entries than two pages hold");

    bool has_empty_page_with_next = false;
    std::optional<uint32_t> offset;
    size_t page_start = 0;

    while(true) {
        const auto response = harness.List(IMAGES, offset);
        zassert_true(response.IsOk());
        zassert_equal(page_start % PAGE, 0);

        // What the page should hold: the assets among the PAGE entries from page_start.
        std::vector<std::pair<std::string, uint32_t>> expected;
        for(size_t i = page_start; i < std::min(page_start + PAGE, raw.size()); i++) {
            if(raw[i].name.starts_with('b'))
                expected.emplace_back(raw[i].name, raw[i].size);
        }

        zassert_equal(response.entries.size(), expected.size(), "Page at %u", static_cast<unsigned>(page_start));
        for(size_t i = 0; i < expected.size(); i++) {
            zassert_str_equal(response.entry_texts[i].at("name").c_str(), expected[i].first.c_str());
            zassert_equal(response.entries[i].at("size"), expected[i].second);
        }

        const bool is_last = page_start + PAGE >= raw.size();
        zassert_equal(response.Has("next"), !is_last, "`next` is there until the directory ends");

        if(is_last)
            break;

        zassert_equal(response.Value("next"), page_start + PAGE);
        has_empty_page_with_next = has_empty_page_with_next || expected.empty();
        page_start += PAGE;
        offset = response.Value("next");
    }

    zassert_true(has_empty_page_with_next, "A page without an asset still says where to go on");
}

ZTEST_F(smp_assets_group, test_list_pages_through_entries_that_do_not_fit_one_response) {
    Harness& harness = *fixture->harness;
    constexpr size_t FILES = 40;

    std::map<std::string, size_t> names;
    for(size_t i = 0; i < FILES; i++) {
        const std::string name = NameOfLength(i, CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH - 4);
        harness.Put("images/" + name, Pattern(i, 1));
        names[name] = i;
    }

    zassert_equal(names.size(), FILES);

    std::set<std::string> listed;
    size_t listed_count = 0;
    size_t pages = 0;
    std::optional<uint32_t> offset;

    while(true) {
        const auto response = harness.List(IMAGES, offset);
        zassert_true(response.IsOk());
        pages++;

        zassert_true(response.entries.size() <= AssetsMgmtGroup::MAX_PAGE_ENTRIES);
        for(size_t i = 0; i < response.entries.size(); i++) {
            const std::string& name = response.entry_texts[i].at("name");
            zassert_true(names.contains(name), "%s is no file", name.c_str());
            zassert_true(listed.insert(name).second, "%s is listed twice", name.c_str());
            zassert_equal(response.entries[i].at("size"), names.at(name));
        }

        listed_count += response.entries.size();

        if(!response.Has("next"))
            break;

        // Only assets are in the directory, so the directory position is the number of entries gone by.
        zassert_equal(response.Value("next"), listed_count);
        zassert_true(!response.entries.empty(), "A page holds at least one of the longest names");
        offset = response.Value("next");
        zassert_true(pages < 100);
    }

    zassert_true(pages > 1, "%u names do not fit one response", static_cast<unsigned>(FILES));
    zassert_equal(listed.size(), names.size(), "Every file is listed once");
}

ZTEST_F(smp_assets_group, test_list_from_an_offset_resumes_in_the_directory) {
    Harness& harness = *fixture->harness;

    for(char letter = 'a'; letter <= 'e'; letter++)
        harness.Put(std::string("images/") + letter + ".bin", Pattern(4, 1));

    const auto raw = harness.RawEntries("images");
    zassert_equal(raw.size(), 5);

    const auto response = harness.List(IMAGES, 2);

    zassert_true(response.IsOk());
    zassert_equal(response.entries.size(), 3);
    for(size_t i = 0; i < 3; i++)
        zassert_str_equal(response.entry_texts[i].at("name").c_str(), raw[2 + i].name.c_str());
    zassert_false(response.Has("next"), "The directory is done");

    const auto past_the_end = harness.List(IMAGES, 99);

    zassert_true(past_the_end.IsOk());
    zassert_true(past_the_end.entries.empty());
    zassert_false(past_the_end.Has("next"));
}

ZTEST_F(smp_assets_group, test_list_of_a_missing_directory_is_empty) {
    Harness& harness = *fixture->harness;

    zassert_true(harness.volume->DeleteRecursive("images"));
    zassert_true(harness.volume->DeleteFile("images"));
    zassert_false(harness.volume->Exists("images"));

    const auto response = harness.List(IMAGES);

    zassert_true(response.IsOk());
    zassert_equal(response.list_sizes.at("entries"), 0);
    zassert_false(response.Has("next"));
}

ZTEST_F(smp_assets_group, test_list_of_an_unknown_or_unavailable_kind_is_an_error) {
    Harness& harness = *fixture->harness;

    AssertError(harness.List(5), Error::UNKNOWN_KIND);
    AssertError(harness.List(256), Error::UNKNOWN_KIND);
    AssertError(harness.List(UINT32_MAX), Error::UNKNOWN_KIND);

    harness.registry->Unregister(AssetKind::LOGS);
    AssertError(harness.List(LOGS), Error::UNKNOWN_KIND);

    AssertError(harness.List(ANIMATIONS), Error::UNAVAILABLE);
    harness.animations_volume->available = true;
    zassert_true(harness.List(ANIMATIONS).IsOk());
}

ZTEST_F(smp_assets_group, test_list_needs_no_access_bit) {
    Harness& harness = *fixture->harness;

    harness.Put("can_scripts/start.lua", Pattern(7, 1));

    // The kind takes uploads only.
    const auto listing = harness.ListAll(CAN_SCRIPTS);

    zassert_equal(listing.files.size(), 1);
    zassert_str_equal(listing.files[0].first.c_str(), "start.lua");
}

// ======== READ ========

ZTEST_F(smp_assets_group, test_read_of_a_small_file_answers_len_and_crc_at_once) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(200, 3);
    harness.Put("images/small.bin", data);

    const auto response = harness.Read(IMAGES, "small.bin", 0);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("off"), 0);
    zassert_equal(response.Value("len"), data.size());
    zassert_equal(response.Value("crc"), Crc(data));
    zassert_true(Harness::DataOf(response) == data);
}

ZTEST_F(smp_assets_group, test_read_in_chunks_of_one_size_gives_the_file_and_its_crc) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE + 123;
    const Bytes data = Pattern(size, 5);
    harness.Put("images/big.bin", data);

    const auto download = harness.DownloadAll(IMAGES, "big.bin", size);

    zassert_true(download.chunks >= 4);
    zassert_true(download.data == data);
    zassert_equal(download.crc, Crc(data));
    zassert_true(download.chunk_size < CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, "A response holds more than its data");
    zassert_true(download.chunk_size > CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE / 2, "A chunk fills most of a response");
}

ZTEST_F(smp_assets_group, test_read_of_a_file_as_big_as_a_chunk_fits_one_response) {
    Harness& harness = *fixture->harness;

    // The longest chunk comes with every key the answer to the first chunk can carry.
    harness.Put("images/probe.bin", Pattern(3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 1));
    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "probe.bin", 0)).size();

    const Bytes data = Pattern(chunk, 2);
    harness.Put("images/exact.bin", data);

    const auto response = harness.Read(IMAGES, "exact.bin", 0);

    zassert_true(
        response.IsOk(), "off, len, data and crc fit next to a chunk of %u bytes", static_cast<unsigned>(chunk)
    );
    zassert_equal(response.Value("off"), 0);
    zassert_equal(response.Value("len"), chunk);
    zassert_equal(response.Value("crc"), Crc(data));
    zassert_true(Harness::DataOf(response) == data);

    // One more byte takes a second chunk.
    const Bytes more = Pattern(chunk + 1, 2);
    harness.Put("images/more.bin", more);

    const auto download = harness.DownloadAll(IMAGES, "more.bin", more.size());

    zassert_equal(download.chunks, 2);
    zassert_true(download.data == more);
    zassert_equal(download.crc, Crc(more));
}

ZTEST_F(smp_assets_group, test_read_of_a_multiple_of_the_chunk_size_ends_with_a_full_chunk) {
    Harness& harness = *fixture->harness;

    harness.Put("images/probe.bin", Pattern(3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 1));
    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "probe.bin", 0)).size();

    const Bytes data = Pattern(3 * chunk, 4);
    harness.Put("images/triple.bin", data);

    const auto download = harness.DownloadAll(IMAGES, "triple.bin", data.size());

    zassert_equal(download.chunks, 3);
    zassert_equal(download.chunk_size, chunk);
    zassert_true(download.data == data);
    zassert_equal(download.crc, Crc(data));

    // At the end: no data, and the CRC, without `len`.
    const auto at_end = harness.Read(IMAGES, "triple.bin", data.size());

    zassert_true(at_end.IsOk());
    zassert_equal(at_end.Value("off"), data.size());
    zassert_true(Harness::DataOf(at_end).empty());
    zassert_false(at_end.Has("len"));
    zassert_equal(at_end.Value("crc"), Crc(data));
}

ZTEST_F(smp_assets_group, test_read_of_an_empty_file) {
    Harness& harness = *fixture->harness;
    harness.Put("images/empty.bin", {});

    const auto response = harness.Read(IMAGES, "empty.bin", 0);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("off"), 0);
    zassert_equal(response.Value("len"), 0);
    zassert_equal(response.Value("crc"), 0);
    zassert_true(Harness::DataOf(response).empty());

    AssertError(harness.Read(IMAGES, "empty.bin", 1), Error::BAD_OFFSET);
}

ZTEST_F(smp_assets_group, test_read_past_the_end_is_a_bad_offset_with_the_length) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 1);
    harness.Put("images/a.bin", data);

    const auto response = harness.Read(IMAGES, "a.bin", 301);

    AssertError(response, Error::BAD_OFFSET);
    zassert_equal(response.Value("len"), 300, "The error says how long the file is");

    // The end itself is no error.
    const auto at_end = harness.Read(IMAGES, "a.bin", 300);
    zassert_true(at_end.IsOk());
    zassert_equal(at_end.Value("crc"), Crc(data));
}

ZTEST_F(smp_assets_group, test_read_past_the_end_keeps_the_session) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 1);
    harness.Put("images/a.bin", data);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    AssertError(harness.Read(IMAGES, "a.bin", size + 1), Error::BAD_OFFSET);
    zassert_equal(atomic_get(&files_closed), closed_before, "The file stays open");

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);

    zassert_equal(download.crc, Crc(data));
}

ZTEST_F(smp_assets_group, test_read_rereading_an_earlier_chunk_keeps_the_crc) {
    Harness& harness = *fixture->harness;
    const size_t size = 4 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE + 50;
    const Bytes data = Pattern(size, 6);
    harness.Put("images/a.bin", data);

    const Bytes first = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0));
    const size_t chunk = first.size();
    const Bytes second = Harness::DataOf(harness.Read(IMAGES, "a.bin", chunk));
    Harness::DataOf(harness.Read(IMAGES, "a.bin", 2 * chunk));

    // The response to the second chunk got lost.
    const auto again = harness.Read(IMAGES, "a.bin", chunk);
    zassert_true(Harness::DataOf(again) == second);
    zassert_false(again.Has("crc"));
    zassert_false(again.Has("len"));

    // And the first one, which the group reopens the file for.
    const auto first_again = harness.Read(IMAGES, "a.bin", 0);
    zassert_true(Harness::DataOf(first_again) == first);
    zassert_equal(first_again.Value("len"), size);

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", 3 * chunk, size);
    zassert_equal(download.crc, Crc(data));
    zassert_true(download.data == Bytes(data.begin() + 3 * chunk, data.end()));
}

ZTEST_F(smp_assets_group, test_read_chunks_asked_out_of_order_give_the_right_crc) {
    Harness& harness = *fixture->harness;
    const size_t size = 5 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 8);
    harness.Put("images/a.bin", data);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();

    // A pipeline lost the request for the second chunk and asks for the third first.
    const auto third = harness.Read(IMAGES, "a.bin", 2 * chunk);
    zassert_true(Harness::DataOf(third) == Bytes(data.begin() + 2 * chunk, data.begin() + 3 * chunk));
    zassert_false(third.Has("crc"));

    const auto second = harness.Read(IMAGES, "a.bin", chunk);
    zassert_true(Harness::DataOf(second) == Bytes(data.begin() + chunk, data.begin() + 2 * chunk));

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", 3 * chunk, size);
    zassert_equal(download.crc, Crc(data));
    zassert_true(download.data == Bytes(data.begin() + 3 * chunk, data.end()));
}

ZTEST_F(smp_assets_group, test_read_resume_on_a_fresh_group_gives_the_right_crc) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(LARGE_FILE_SIZE, 11);
    harness.Put("images/big.bin", data);

    // A download reaches the middle, and the unit restarts.
    size_t offset = 0;
    while(offset < LARGE_FILE_SIZE / 2)
        offset += Harness::DataOf(harness.Read(IMAGES, "big.bin", offset)).size();

    harness.StartGroup();

    const auto download = harness.DownloadFrom(IMAGES, "big.bin", offset, data.size());

    zassert_equal(download.crc, Crc(data), "The part before the offset is read for the CRC");
    zassert_true(download.data == Bytes(data.begin() + offset, data.end()));
}

ZTEST_F(smp_assets_group, test_read_resume_after_a_timeout_continues_from_the_checkpoint) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(LARGE_FILE_SIZE, 12);
    harness.Put("images/big.bin", data);

    size_t offset = 0;
    while(offset < LARGE_FILE_SIZE / 2)
        offset += Harness::DataOf(harness.Read(IMAGES, "big.bin", offset)).size();

    // The client stops asking.
    const atomic_val_t closed_before = atomic_get(&files_closed);
    k_msleep(PAST_TIMEOUT_MS);
    zassert_true(atomic_get(&files_closed) > closed_before, "The idle session closes its file");

    atomic_set(&bytes_read_from_files, 0);

    const auto download = harness.DownloadFrom(IMAGES, "big.bin", offset, data.size());

    zassert_equal(download.crc, Crc(data));
    zassert_true(download.data == Bytes(data.begin() + offset, data.end()));

    // Without the checkpoint the group would read the file from its start for the CRC. It reads from
    // the file block that holds the offset instead.
    const size_t read = static_cast<size_t>(atomic_get(&bytes_read_from_files));
    zassert_true(
        read <= data.size() - AssetReader::BUFFER_SIZE, "The resume read %u of %u bytes", static_cast<unsigned>(read),
        static_cast<unsigned>(data.size())
    );
}

ZTEST_F(smp_assets_group, test_read_resume_of_another_file_does_not_use_the_checkpoint) {
    Harness& harness = *fixture->harness;
    const Bytes first = Pattern(LARGE_FILE_SIZE, 13);
    const Bytes second = Pattern(LARGE_FILE_SIZE, 14);
    harness.Put("images/first.bin", first);
    harness.Put("images/second.bin", second);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "first.bin", 0)).size();

    // The session of the first file closes for the second one and leaves its checkpoint. The second
    // file has to be read for its CRC.
    const auto download = harness.DownloadFrom(IMAGES, "second.bin", chunk, second.size());

    zassert_equal(download.crc, Crc(second));

    const auto rest = harness.DownloadFrom(IMAGES, "first.bin", chunk, first.size());

    zassert_equal(rest.crc, Crc(first));
}

ZTEST_F(smp_assets_group, test_read_of_another_file_replaces_the_session) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE + 7;
    const Bytes first = Pattern(size, 20);
    const Bytes second = Pattern(size + 100, 21);
    harness.Put("images/first.bin", first);
    harness.Put("images/second.bin", second);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "first.bin", 0)).size();
    zassert_equal(Harness::DataOf(harness.Read(IMAGES, "second.bin", 0)).size(), chunk);

    // Back and forth, one chunk at a time.
    Bytes first_read;
    Bytes second_read;
    std::optional<uint32_t> first_crc;
    std::optional<uint32_t> second_crc;

    for(size_t offset = chunk; offset < second.size(); offset += chunk) {
        if(offset < first.size()) {
            const auto response = harness.Read(IMAGES, "first.bin", offset);
            const Bytes& data = Harness::DataOf(response);
            first_read.insert(first_read.end(), data.begin(), data.end());
            if(response.Has("crc"))
                first_crc = response.Value("crc");
        }

        const auto response = harness.Read(IMAGES, "second.bin", offset);
        const Bytes& data = Harness::DataOf(response);
        second_read.insert(second_read.end(), data.begin(), data.end());
        if(response.Has("crc"))
            second_crc = response.Value("crc");
    }

    zassert_true(first_read == Bytes(first.begin() + chunk, first.end()));
    zassert_true(second_read == Bytes(second.begin() + chunk, second.end()));
    zassert_true(first_crc.has_value() && *first_crc == Crc(first));
    zassert_true(second_crc.has_value() && *second_crc == Crc(second));
}

ZTEST_F(smp_assets_group, test_read_of_many_files_in_turn_releases_each_file) {
    Harness& harness = *fixture->harness;

    // More files than the file system has slots, each left open by its first chunk.
    for(size_t i = 0; i < 12; i++) {
        const std::string name = "f" + std::to_string(i) + ".bin";
        harness.Put("images/" + name, Pattern(2 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, static_cast<uint8_t>(i)));

        const auto response = harness.Read(IMAGES, name, 0);

        zassert_true(response.IsOk(), "Read of %s failed with %u", name.c_str(), response.error.value_or(0));
    }
}

ZTEST_F(smp_assets_group, test_read_of_a_missing_file_is_not_found) {
    Harness& harness = *fixture->harness;

    AssertError(harness.Read(IMAGES, "none.bin", 0), Error::NOT_FOUND);
    AssertError(harness.Read(IMAGES, "none.bin", 100), Error::NOT_FOUND);

    // A directory is no asset to read.
    zassert_true(harness.volume->CreateDirectory("images/folder.png"));
    AssertError(harness.Read(IMAGES, "folder.png", 0), Error::NOT_FOUND);

    // The group answers again.
    harness.Put("images/a.bin", Pattern(5, 1));
    zassert_true(harness.Read(IMAGES, "a.bin", 0).IsOk());
}

ZTEST_F(smp_assets_group, test_read_of_a_bad_name_is_a_bad_name) {
    Harness& harness = *fixture->harness;
    harness.Put("images/a.bin", Pattern(5, 1));
    harness.Put("images/x.txt", Pattern(5, 1));
    harness.Put("scripts/a.lua", Pattern(5, 1));

    AssertError(harness.Read(IMAGES, "../a.bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "../scripts/a.lua", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "/images/a.bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "sub/a.bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "x.txt", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "a.lua", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, ".bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, ".a.bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, "a b.bin", 0), Error::BAD_NAME);
    AssertError(harness.Read(IMAGES, std::string_view("a\0.bin", 6), 0), Error::BAD_NAME);
    AssertError(
        harness.Read(IMAGES, std::string(CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH, 'n') + ".bin", 0), Error::BAD_NAME
    );

    // The extension of another kind's file.
    AssertError(harness.Read(SENSOR_SCRIPTS, "a.bin", 0), Error::BAD_NAME);

    zassert_true(harness.Read(IMAGES, "a.bin", 0).IsOk());
    zassert_true(harness.Read(SENSOR_SCRIPTS, "a.lua", 0).IsOk());
}

ZTEST_F(smp_assets_group, test_read_of_an_unknown_kind_or_one_without_the_read_bit_is_refused) {
    Harness& harness = *fixture->harness;
    harness.Put("can_scripts/a.lua", Pattern(5, 1));

    AssertError(harness.Read(5, "a.bin", 0), Error::UNKNOWN_KIND);
    AssertError(harness.Read(256, "a.bin", 0), Error::UNKNOWN_KIND);
    AssertError(harness.Read(UINT32_MAX, "a.bin", 0), Error::UNKNOWN_KIND);

    AssertError(harness.Read(CAN_SCRIPTS, "a.lua", 0), Error::DENIED);

    harness.registry->Unregister(AssetKind::IMAGES);
    AssertError(harness.Read(IMAGES, "a.bin", 0), Error::UNKNOWN_KIND);
}

ZTEST_F(smp_assets_group, test_read_checks_the_kind_then_the_access_then_the_name_then_the_volume) {
    Harness& harness = *fixture->harness;

    // The kind takes no reads: even a bad name is answered by the access.
    AssertError(harness.Read(CAN_SCRIPTS, "../a", 0), Error::DENIED);
    // The volume is not mounted: a bad name comes first.
    AssertError(harness.Read(ANIMATIONS, "../a", 0), Error::BAD_NAME);
    AssertError(harness.Read(ANIMATIONS, "a.json", 0), Error::UNAVAILABLE);
}

ZTEST_F(smp_assets_group, test_read_of_an_unmounted_volume_is_unavailable) {
    Harness& harness = *fixture->harness;
    harness.Put("animations/a.json", Pattern(5, 1));

    AssertError(harness.Read(ANIMATIONS, "a.json", 0), Error::UNAVAILABLE);

    harness.animations_volume->available = true;
    zassert_true(harness.Read(ANIMATIONS, "a.json", 0).IsOk());
}

ZTEST_F(smp_assets_group, test_read_closes_the_file_when_its_volume_goes_away) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 15);
    harness.Put("images/a.bin", data);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();

    harness.images_volume->available = false;
    const atomic_val_t closed_before = atomic_get(&files_closed);

    AssertError(harness.Read(IMAGES, "a.bin", chunk), Error::UNAVAILABLE);
    zassert_equal(atomic_get(&files_closed), closed_before + 1, "The open file is closed");

    // The volume is back: the transfer goes on, from the checkpoint.
    harness.images_volume->available = true;

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);

    zassert_equal(download.crc, Crc(data));
    zassert_true(download.data == Bytes(data.begin() + chunk, data.end()));
}

ZTEST_F(smp_assets_group, test_read_of_a_file_in_use_is_refused_when_the_session_opens) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 16);
    harness.Put("scripts/busy.lua", data);
    harness.in_use.names.insert("busy.lua");

    AssertError(harness.Read(SENSOR_SCRIPTS, "busy.lua", 0), Error::IN_USE);
    zassert_equal(harness.in_use.calls.size(), 1);
    zassert_true(harness.in_use.calls[0].first == AssetAccess::READ);
    zassert_str_equal(harness.in_use.calls[0].second.c_str(), "busy.lua");

    // The refusal leaves no session behind: a request in the middle asks again.
    AssertError(harness.Read(SENSOR_SCRIPTS, "busy.lua", 10), Error::IN_USE);
    zassert_equal(harness.in_use.calls.size(), 2);

    harness.in_use.names.clear();
    const size_t chunk = Harness::DataOf(harness.Read(SENSOR_SCRIPTS, "busy.lua", 0)).size();

    // A file taken into use during a download is read to its end.
    harness.in_use.names.insert("busy.lua");
    const size_t calls = harness.in_use.calls.size();

    const auto download = harness.DownloadFrom(SENSOR_SCRIPTS, "busy.lua", chunk, size);

    zassert_equal(download.crc, Crc(data));
    zassert_equal(harness.in_use.calls.size(), calls, "The check is made when a session opens only");

    // A new download asks again.
    AssertError(harness.Read(SENSOR_SCRIPTS, "busy.lua", 0), Error::IN_USE);
}

ZTEST_F(smp_assets_group, test_a_request_with_a_missing_or_malformed_key_is_invalid) {
    Harness& harness = *fixture->harness;
    harness.Put("images/a.bin", Pattern(5, 1));

    const uint8_t read_command = std::to_underlying(Command::READ);
    const uint8_t list_command = std::to_underlying(Command::LIST);
    const uint8_t delete_command = std::to_underlying(Command::DELETE);

    const auto client_read = [&](uint8_t command, const Bytes& body) {
        return harness.client.Read(SmpGroupId::ASSETS, command, body);
    };

    zassert_equal(
        client_read(read_command, CborMap().PutText("name", "a.bin").Put("off", 0).Build()).Value("rc"), MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_read(read_command, CborMap().Put("kind", IMAGES).Put("off", 0).Build()).Value("rc"), MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_read(read_command, CborMap().Put("kind", IMAGES).PutText("name", "a.bin").Build()).Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_read(read_command, CborMap().PutText("kind", "images").PutText("name", "a.bin").Put("off", 0).Build())
            .Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_read(read_command, CborMap().Put("kind", IMAGES).Put("name", 7).Put("off", 0).Build()).Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(client_read(list_command, CborMap().Build()).Value("rc"), MGMT_ERR_EINVAL);

    const auto no_name = harness.client.Write(
        SmpGroupId::ASSETS, delete_command, CborMap().Put("kind", IMAGES).Build()
    );
    zassert_equal(no_name.Value("rc"), MGMT_ERR_EINVAL);
}

ZTEST_F(smp_assets_group, test_read_from_the_start_recomputes_the_crc_of_a_file_that_changed) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE + 5;
    const Bytes old_data = Pattern(size, 1);
    const Bytes new_data = Pattern(size, 2);
    harness.Put("images/a.bin", old_data);

    const auto first = harness.DownloadAll(IMAGES, "a.bin", size);

    zassert_equal(first.crc, Crc(old_data));

    // The file is written over in place, by someone else (a card edited on a PC), at the same size.
    harness.Put("images/a.bin", new_data);

    const auto second = harness.DownloadAll(IMAGES, "a.bin", size);

    zassert_true(second.data == new_data);
    zassert_equal(second.crc, Crc(new_data), "A download from the start is not seeded with the old CRC");

    // And that one is what is kept now.
    const auto third = harness.DownloadAll(IMAGES, "a.bin", size);

    zassert_equal(third.crc, Crc(new_data));
}

ZTEST_F(smp_assets_group, test_read_whose_in_use_check_throws_is_no_memory_and_leaves_no_session) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data_a = Pattern(size, 1);
    const Bytes data_b = Pattern(size + 40, 2);
    harness.Put("scripts/a.lua", data_a);
    harness.Put("scripts/b.lua", data_b);

    // A session with something to go stale: a's progress.
    const size_t chunk = Harness::DataOf(harness.Read(SENSOR_SCRIPTS, "a.lua", 0)).size();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    harness.in_use.throws_bad_alloc = true;
    AssertError(harness.Read(SENSOR_SCRIPTS, "b.lua", 0), Error::NO_MEMORY);
    harness.in_use.throws_bad_alloc = false;

    // The file of a is closed for the new one, and the new one is closed when the check fails.
    zassert_equal(atomic_get(&files_closed), closed_before + 2, "No file stays open");

    // The failed read left no state behind that a later one could take for b's.
    const auto download_b = harness.DownloadFrom(SENSOR_SCRIPTS, "b.lua", chunk, data_b.size());

    zassert_equal(download_b.crc, Crc(data_b));
    zassert_true(download_b.data == Bytes(data_b.begin() + chunk, data_b.end()));

    const auto whole_b = harness.DownloadAll(SENSOR_SCRIPTS, "b.lua", data_b.size());

    zassert_equal(whole_b.crc, Crc(data_b));

    const auto download_a = harness.DownloadFrom(SENSOR_SCRIPTS, "a.lua", chunk, data_a.size());

    zassert_equal(download_a.crc, Crc(data_a));
}

ZTEST_F(smp_assets_group, test_a_destroyed_group_closes_its_session_and_goes_quiet) {
    Harness& harness = *fixture->harness;
    harness.Put("images/a.bin", Pattern(3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 1));

    zassert_true(harness.Read(IMAGES, "a.bin", 0).IsOk());
    const atomic_val_t closed_before = atomic_get(&files_closed);

    // The session's idle task is armed now.
    harness.group.reset();
    zassert_equal(atomic_get(&files_closed), closed_before + 1);

    // Nothing of the group runs after it: its idle task would use freed memory.
    k_msleep(PAST_TIMEOUT_MS);
    zassert_equal(atomic_get(&files_closed), closed_before + 1);
}

// ======== DELETE ========

ZTEST_F(smp_assets_group, test_delete_removes_the_file) {
    Harness& harness = *fixture->harness;
    harness.Put("images/a.bin", Pattern(50, 1));
    harness.Put("images/b.bin", Pattern(50, 2));

    const auto response = harness.Delete(IMAGES, "a.bin");

    zassert_true(response.IsOk());
    zassert_false(harness.volume->Exists("images/a.bin"));
    zassert_true(harness.volume->Exists("images/b.bin"), "Only that file");

    AssertError(harness.Delete(IMAGES, "a.bin"), Error::NOT_FOUND);
    AssertError(harness.Read(IMAGES, "a.bin", 0), Error::NOT_FOUND);
}

ZTEST_F(smp_assets_group, test_delete_of_a_directory_is_not_found) {
    Harness& harness = *fixture->harness;
    zassert_true(harness.volume->CreateDirectory("images/folder.png"));

    AssertError(harness.Delete(IMAGES, "folder.png"), Error::NOT_FOUND);
    zassert_true(harness.volume->Exists("images/folder.png"));
}

ZTEST_F(smp_assets_group, test_delete_without_the_delete_bit_is_denied) {
    Harness& harness = *fixture->harness;
    harness.Put("logs/run.mdf", Pattern(50, 1));
    harness.Put("can_scripts/a.lua", Pattern(50, 1));

    AssertError(harness.Delete(LOGS, "run.mdf"), Error::DENIED);
    AssertError(harness.Delete(CAN_SCRIPTS, "a.lua"), Error::DENIED);
    zassert_true(harness.volume->Exists("logs/run.mdf"));
    zassert_true(harness.volume->Exists("can_scripts/a.lua"));
}

ZTEST_F(smp_assets_group, test_delete_of_a_bad_name_or_an_unknown_kind_is_refused) {
    Harness& harness = *fixture->harness;
    harness.Put("images/a.bin", Pattern(50, 1));
    harness.Put("a.bin", Pattern(50, 2));

    AssertError(harness.Delete(IMAGES, "../a.bin"), Error::BAD_NAME);
    AssertError(harness.Delete(IMAGES, "images/a.bin"), Error::BAD_NAME);
    AssertError(harness.Delete(IMAGES, "a.txt"), Error::BAD_NAME);
    AssertError(harness.Delete(IMAGES, ""), Error::BAD_NAME);
    AssertError(harness.Delete(5, "a.bin"), Error::UNKNOWN_KIND);
    AssertError(harness.Delete(256, "a.bin"), Error::UNKNOWN_KIND);

    zassert_true(harness.volume->Exists("images/a.bin"));
    zassert_true(harness.volume->Exists("a.bin"), "The volume's root is out of reach");
}

ZTEST_F(smp_assets_group, test_delete_of_a_file_in_use_is_refused) {
    Harness& harness = *fixture->harness;
    harness.Put("scripts/busy.lua", Pattern(50, 1));
    harness.in_use.names.insert("busy.lua");

    AssertError(harness.Delete(SENSOR_SCRIPTS, "busy.lua"), Error::IN_USE);
    zassert_true(harness.volume->Exists("scripts/busy.lua"));
    zassert_equal(harness.in_use.calls.size(), 1);
    zassert_true(harness.in_use.calls[0].first == AssetAccess::DELETE);
    zassert_str_equal(harness.in_use.calls[0].second.c_str(), "busy.lua");

    harness.in_use.names.clear();
    zassert_true(harness.Delete(SENSOR_SCRIPTS, "busy.lua").IsOk());
    zassert_false(harness.volume->Exists("scripts/busy.lua"));
}

ZTEST_F(smp_assets_group, test_delete_of_a_missing_file_does_not_ask_whether_it_is_in_use) {
    Harness& harness = *fixture->harness;
    harness.in_use.names.insert("none.lua");

    AssertError(harness.Delete(SENSOR_SCRIPTS, "none.lua"), Error::NOT_FOUND);
    zassert_true(harness.in_use.calls.empty());
}

ZTEST_F(smp_assets_group, test_delete_on_an_unmounted_volume_is_unavailable) {
    Harness& harness = *fixture->harness;
    harness.Put("animations/a.json", Pattern(50, 1));

    AssertError(harness.Delete(ANIMATIONS, "a.json"), Error::UNAVAILABLE);
    zassert_true(harness.volume->Exists("animations/a.json"));

    harness.animations_volume->available = true;
    zassert_true(harness.Delete(ANIMATIONS, "a.json").IsOk());
    zassert_false(harness.volume->Exists("animations/a.json"));
}

ZTEST_F(smp_assets_group, test_delete_closes_the_read_session_of_the_file) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    harness.Put("images/a.bin", Pattern(size, 1));

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    zassert_true(harness.Delete(IMAGES, "a.bin").IsOk());
    zassert_equal(
        atomic_get(&files_closed), closed_before + 1,
        "The file is closed before it is deleted: FatFs does not lock open files"
    );

    // The next chunk finds no file, not a session on a deleted one.
    AssertError(harness.Read(IMAGES, "a.bin", chunk), Error::NOT_FOUND);
}

ZTEST_F(smp_assets_group, test_delete_of_another_file_leaves_the_read_session_alone) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 2);
    harness.Put("images/a.bin", data);
    harness.Put("images/b.bin", Pattern(10, 3));

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    zassert_true(harness.Delete(IMAGES, "b.bin").IsOk());
    zassert_equal(atomic_get(&files_closed), closed_before, "The download is still open");

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);
    zassert_equal(download.crc, Crc(data));
}

ZTEST_F(smp_assets_group, test_delete_forgets_the_checkpoint_of_the_file) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes old_data = Pattern(size, 1);
    const Bytes new_data = Pattern(size, 2);
    harness.Put("images/a.bin", old_data);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();

    // The file is replaced by one of the same size and a different content.
    zassert_true(harness.Delete(IMAGES, "a.bin").IsOk());
    harness.Put("images/a.bin", new_data);

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);

    zassert_equal(download.crc, Crc(new_data), "The CRC of the old content is not kept");
}

ZTEST_F(smp_assets_group, test_delete_closes_a_session_on_the_same_name_in_another_case) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes old_data = Pattern(size, 1);
    const Bytes new_data = Pattern(size, 2);

    // FAT would call these one file. On the littlefs test volume they are two, and the group still has
    // to treat the one it deletes as the one it may have open: it cannot tell the volumes apart.
    harness.Put("images/logo.bin", old_data);
    harness.Put("images/LOGO.BIN", Pattern(10, 3));

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "logo.bin", 0)).size();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    zassert_true(harness.Delete(IMAGES, "LOGO.BIN").IsOk());
    zassert_equal(atomic_get(&files_closed), closed_before + 1, "The open file is closed before a delete");
    zassert_false(harness.volume->Exists("images/LOGO.BIN"));
    zassert_true(harness.volume->Exists("images/logo.bin"));

    // The checkpoint of the file that was open is gone as well: its content changes now.
    harness.Put("images/logo.bin", new_data);

    const auto download = harness.DownloadFrom(IMAGES, "logo.bin", chunk, size);

    zassert_equal(download.crc, Crc(new_data), "The CRC of the old content is not kept");
}

ZTEST_F(smp_assets_group, test_delete_closes_a_session_opened_through_a_kind_registered_since) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    harness.Put("images/a.bin", Pattern(size, 1));

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();

    harness.ReregisterImages();
    const atomic_val_t closed_before = atomic_get(&files_closed);

    zassert_true(harness.Delete(IMAGES, "a.bin").IsOk());
    zassert_equal(atomic_get(&files_closed), closed_before + 1, "The provider is another one, the file is not");

    AssertError(harness.Read(IMAGES, "a.bin", chunk), Error::NOT_FOUND);
}

ZTEST_F(smp_assets_group, test_a_session_of_a_kind_registered_since_is_not_continued) {
    Harness& harness = *fixture->harness;
    const size_t size = 3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
    const Bytes data = Pattern(size, 4);
    harness.Put("images/a.bin", data);

    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();

    harness.ReregisterImages();

    // The next chunk opens the file through the provider that is registered now, and is right.
    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);

    zassert_equal(download.crc, Crc(data));
    zassert_true(download.data == Bytes(data.begin() + chunk, data.end()));
}

// ======== WRITE ========

ZTEST_F(smp_assets_group, test_write_stores_a_file_sent_in_one_chunk) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 3);

    const auto response = harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data));

    zassert_true(response.IsOk());
    zassert_equal(response.Value("off"), data.size());
    zassert_true(response.Has("tok"), "The answer to the first chunk carries the token");
    zassert_true(harness.FileContent("images/one.bin") == data);
    zassert_false(harness.IsStaged());

    // The group serves what it stored.
    const auto download = harness.DownloadAll(IMAGES, "one.bin", data.size());

    zassert_true(download.data == data);
    zassert_equal(download.crc, Crc(data));
}

ZTEST_F(smp_assets_group, test_write_stores_a_file_sent_in_chunks) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(4 * UPLOAD_CHUNK_SIZE + 77, 4);

    const auto response = harness.UploadAll(IMAGES, "many.bin", data);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("off"), data.size());
    zassert_false(response.Has("tok"), "The token answers the first chunk only");
    zassert_true(harness.FileContent("images/many.bin") == data);
    zassert_false(harness.IsStaged());

    const auto listing = harness.ListAll(IMAGES);

    zassert_equal(listing.files.size(), 1);
    zassert_str_equal(listing.files[0].first.c_str(), "many.bin");
    zassert_equal(listing.files[0].second, data.size());
}

ZTEST_F(smp_assets_group, test_write_takes_a_file_of_the_largest_size_of_the_kind) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(IMAGES_MAX_SIZE, 6);

    zassert_true(harness.UploadAll(IMAGES, "max.bin", data).IsOk());
    zassert_true(harness.FileContent("images/max.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_to_a_kind_that_takes_uploads_only) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 7);

    zassert_true(harness.WriteFirst(CAN_SCRIPTS, "start.lua", data, data.size(), Crc(data)).IsOk());
    zassert_true(harness.FileContent("can_scripts/start.lua") == data);
    AssertError(harness.Read(CAN_SCRIPTS, "start.lua", 0), Error::DENIED);
}

ZTEST_F(smp_assets_group, test_write_replaces_the_file_only_with_the_last_chunk) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(1000, 1);
    const Bytes new_data = Pattern(2 * UPLOAD_CHUNK_SIZE + 100, 2);
    harness.Put("images/a.bin", old_data);

    const auto response = harness.UploadAll(IMAGES, "a.bin", new_data, [&harness, &old_data] {
        // All but the last chunk are in: the staged file holds them and the asset is as it was.
        zassert_true(harness.IsStaged());
        zassert_true(harness.FileContent("images/a.bin") == old_data);
    });

    zassert_true(response.IsOk());
    zassert_true(harness.FileContent("images/a.bin") == new_data);
    zassert_false(harness.IsStaged());
    zassert_equal(harness.ListAll(IMAGES).files.size(), 1);
}

ZTEST_F(smp_assets_group, test_write_of_an_empty_file) {
    Harness& harness = *fixture->harness;
    harness.Put("images/was_full.bin", Pattern(100, 1));

    const auto created = harness.WriteFirst(IMAGES, "new_empty.bin", {}, 0, 0);

    zassert_true(created.IsOk());
    zassert_equal(created.Value("off"), 0);
    zassert_true(created.Has("tok"));
    zassert_equal(harness.volume->GetFileSize("images/new_empty.bin").value_or(99), 0);

    const auto replaced = harness.WriteFirst(IMAGES, "was_full.bin", {}, 0, 0);

    zassert_true(replaced.IsOk());
    zassert_equal(harness.volume->GetFileSize("images/was_full.bin").value_or(99), 0);
    zassert_false(harness.IsStaged());

    // A CRC other than that of nothing is a mismatch.
    harness.Put("images/keep.bin", Pattern(10, 1));
    AssertError(harness.WriteFirst(IMAGES, "keep.bin", {}, 0, 1), Error::CRC_MISMATCH);
    zassert_equal(harness.volume->GetFileSize("images/keep.bin").value_or(99), 10);
    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_write_with_a_wrong_crc_leaves_the_file_and_no_staged_file) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(500, 1);
    const Bytes new_data = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);
    harness.Put("images/a.bin", old_data);

    const auto response = harness.UploadAll(IMAGES, "a.bin", new_data, {}, Crc(new_data) + 1);

    AssertError(response, Error::CRC_MISMATCH);
    zassert_true(harness.FileContent("images/a.bin") == old_data, "The asset is untouched");
    zassert_false(harness.IsStaged());

    // The session is gone with it.
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, 1, Pattern(10, 1)), Error::BAD_TOKEN);

    // Another upload starts at once.
    zassert_true(harness.UploadAll(IMAGES, "a.bin", new_data).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == new_data);
}

ZTEST_F(smp_assets_group, test_write_is_busy_while_another_upload_is_open) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const Bytes other = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);
    const auto first = std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE);
    const auto other_first = std::span<const uint8_t>(other).first(UPLOAD_CHUNK_SIZE);

    const auto started = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    // Another file, another content or length of the same file, another kind: none is the same upload.
    AssertError(harness.WriteFirst(IMAGES, "b.bin", other_first, other.size(), Crc(other)), Error::BUSY);
    AssertError(harness.WriteFirst(IMAGES, "a.bin", other_first, data.size(), Crc(data)), Error::BUSY);
    AssertError(harness.WriteFirst(IMAGES, "a.bin", first, data.size() - 1, Crc(data)), Error::BUSY);
    AssertError(harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data) + 1), Error::BUSY);
    AssertError(harness.WriteFirst(SENSOR_SCRIPTS, "a.lua", first, data.size(), Crc(data)), Error::BUSY);

    // Another client's token is none of the session's.
    AssertError(harness.WriteFirst(IMAGES, "a.bin", other_first, data.size(), Crc(data), token + 1), Error::BUSY);

    // The upload that is open is as it was.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, first.size()).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
    zassert_false(harness.volume->Exists("images/b.bin"));
}

ZTEST_F(smp_assets_group, test_write_first_chunk_sent_again_gets_the_token_again) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const auto first = std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE);

    const auto started = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    // The answer got lost: the same request, without the token and with another one.
    const auto again = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));

    zassert_true(again.IsOk());
    zassert_equal(again.Value("off"), UPLOAD_CHUNK_SIZE);
    zassert_equal(again.Value("tok"), token);

    const auto with_token = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data), token + 1);

    zassert_true(with_token.IsOk());
    zassert_equal(with_token.Value("off"), UPLOAD_CHUNK_SIZE);
    zassert_equal(with_token.Value("tok"), token);

    // The data was not taken twice.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, UPLOAD_CHUNK_SIZE).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_first_chunk_sent_again_after_more_data_is_busy) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const auto first = std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE);

    const auto started = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    zassert_true(harness
            .WriteNext(
                IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token,
                std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
            )
            .IsOk());

    AssertError(harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data)), Error::BUSY);

    // The upload goes on.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, 2 * UPLOAD_CHUNK_SIZE).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_owner_starts_over_with_its_token) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const Bytes other = Pattern(2 * UPLOAD_CHUNK_SIZE, 2);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    // The same name, another file, with the token.
    const auto restarted = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(other).first(400), other.size(), Crc(other), token
    );

    zassert_true(restarted.IsOk());
    zassert_equal(restarted.Value("off"), 400);
    zassert_true(restarted.Has("tok"));
    const uint32_t new_token = restarted.Value("tok");

    // There is one staged file, and the upload is where the restart left it.
    const auto wrong_offset = harness.WriteNext(
        IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, new_token, std::span<const uint8_t>(other).subspan(UPLOAD_CHUNK_SIZE, 10)
    );
    AssertError(wrong_offset, Error::BAD_OFFSET);
    zassert_equal(wrong_offset.Value("off"), 400);

    if(new_token != token) {
        AssertError(
            harness.WriteNext(IMAGES, "a.bin", 400, token, std::span<const uint8_t>(other).subspan(400, 100)),
            Error::BAD_TOKEN
        );
    }

    zassert_true(harness.SendRest(IMAGES, "a.bin", other, new_token, 400).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == other);
    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_write_chunks_without_a_matching_session_are_bad_token) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const auto first = std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE);
    const auto second = std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE);

    // No upload is open.
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, 1, second), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, std::nullopt, second), Error::BAD_TOKEN);

    const auto started = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, std::nullopt, second), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token + 1, second), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(IMAGES, "b.bin", UPLOAD_CHUNK_SIZE, token, second), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(SENSOR_SCRIPTS, "a.bin", UPLOAD_CHUNK_SIZE, token, second), Error::BAD_TOKEN);

    // None of them touched the upload.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, UPLOAD_CHUNK_SIZE).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_chunk_at_the_wrong_offset_says_where_to_go_on) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const auto first = std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE);

    const auto started = harness.WriteFirst(IMAGES, "a.bin", first, data.size(), Crc(data));
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");
    const auto second = std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE);

    // Ahead of the data received, and behind it (the answer to a chunk got lost, and it is sent again).
    const auto ahead = harness.WriteNext(IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, token, second);
    AssertError(ahead, Error::BAD_OFFSET);
    zassert_equal(ahead.Value("off"), UPLOAD_CHUNK_SIZE);

    zassert_true(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, second).IsOk());

    const auto behind = harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, second);
    AssertError(behind, Error::BAD_OFFSET);
    zassert_equal(behind.Value("off"), 2 * UPLOAD_CHUNK_SIZE);

    // The session is as it was.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, 2 * UPLOAD_CHUNK_SIZE).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_larger_than_the_kind_takes_is_too_large) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(IMAGES_MAX_SIZE + 1, 1);
    const Bytes small = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);

    AssertError(harness.UploadAll(IMAGES, "big.bin", data), Error::TOO_LARGE);
    zassert_false(harness.IsStaged());
    AssertError(harness.WriteNext(IMAGES, "big.bin", UPLOAD_CHUNK_SIZE, 1, Pattern(10, 1)), Error::BAD_TOKEN);

    // It is the first thing looked at once the kind and the name are known: another upload's session
    // does not make it busy.
    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(small).first(UPLOAD_CHUNK_SIZE), small.size(), Crc(small)
    );
    zassert_true(started.IsOk());

    AssertError(harness.UploadAll(IMAGES, "big.bin", data), Error::TOO_LARGE);
    zassert_true(harness.SendRest(IMAGES, "a.bin", small, started.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());
}

ZTEST_F(smp_assets_group, test_write_with_more_data_than_it_declares_is_too_large) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2 * UPLOAD_CHUNK_SIZE, 1);

    // In the first chunk.
    AssertError(
        harness.WriteFirst(IMAGES, "a.bin", std::span<const uint8_t>(data).first(100), 50, 0), Error::TOO_LARGE
    );
    zassert_false(harness.IsStaged());

    // In a later one: the upload is aborted.
    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), UPLOAD_CHUNK_SIZE + 100, Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, 101)
        ),
        Error::TOO_LARGE
    );
    zassert_false(harness.IsStaged());
    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, 100)
        ),
        Error::BAD_TOKEN
    );
    zassert_false(harness.volume->Exists("images/a.bin"));
}

ZTEST_F(smp_assets_group, test_write_checks_the_kind_the_access_the_name_the_volume_and_the_size_in_that_order) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(100, 1);

    AssertError(harness.WriteFirst(5, "a.bin", data, data.size(), Crc(data)), Error::UNKNOWN_KIND);
    AssertError(harness.WriteFirst(256, "a.bin", data, data.size(), Crc(data)), Error::UNKNOWN_KIND);

    // A kind that takes no writes: even a bad name is answered by the access.
    AssertError(harness.WriteFirst(LOGS, "../a", data, data.size(), Crc(data)), Error::DENIED);
    AssertError(harness.WriteFirst(LOGS, "run.mdf", data, data.size(), Crc(data)), Error::DENIED);

    AssertError(harness.WriteFirst(IMAGES, "../a.bin", data, data.size(), Crc(data)), Error::BAD_NAME);
    AssertError(harness.WriteFirst(IMAGES, "a.txt", data, data.size(), Crc(data)), Error::BAD_NAME);
    AssertError(harness.WriteFirst(IMAGES, "", data, data.size(), Crc(data)), Error::BAD_NAME);
    AssertError(harness.WriteFirst(IMAGES, ".hidden.bin", data, data.size(), Crc(data)), Error::BAD_NAME);

    // The volume is not mounted: the name comes first, the size after.
    AssertError(harness.WriteFirst(ANIMATIONS, "../a", data, data.size(), Crc(data)), Error::BAD_NAME);
    AssertError(harness.WriteFirst(ANIMATIONS, "a.json", data, ANIMATIONS_MAX_SIZE + 1, Crc(data)), Error::UNAVAILABLE);

    // The size comes before the check whether the file is in use.
    harness.in_use.names.insert("busy.lua");
    AssertError(
        harness.WriteFirst(SENSOR_SCRIPTS, "busy.lua", data, SCRIPTS_MAX_SIZE + 1, Crc(data)), Error::TOO_LARGE
    );
    zassert_true(harness.in_use.calls.empty());

    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_write_of_a_file_in_use_is_refused) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 1);
    harness.in_use.names.insert("busy.lua");

    AssertError(harness.WriteFirst(SENSOR_SCRIPTS, "busy.lua", data, data.size(), Crc(data)), Error::IN_USE);
    zassert_equal(harness.in_use.calls.size(), 1);
    zassert_true(harness.in_use.calls[0].first == AssetAccess::WRITE);
    zassert_str_equal(harness.in_use.calls[0].second.c_str(), "busy.lua");
    zassert_false(harness.IsStaged());
    AssertError(harness.WriteNext(SENSOR_SCRIPTS, "busy.lua", 100, 1, data), Error::BAD_TOKEN);

    // It asks again, and a file that is not in use is stored.
    harness.in_use.names.clear();
    harness.in_use.calls.clear();

    zassert_true(harness.WriteFirst(SENSOR_SCRIPTS, "busy.lua", data, data.size(), Crc(data)).IsOk());
    zassert_true(harness.FileContent("scripts/busy.lua") == data);

    // Before the first chunk, and again before the file is replaced.
    zassert_equal(harness.in_use.calls.size(), 2);
    zassert_true(harness.in_use.calls[0].first == AssetAccess::WRITE);
    zassert_true(harness.in_use.calls[1].first == AssetAccess::WRITE);
}

ZTEST_F(smp_assets_group, test_write_of_a_file_that_went_into_use_during_the_upload_is_refused) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(100, 1);
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);
    harness.Put("scripts/a.lua", old_data);

    const auto response = harness.UploadAll(SENSOR_SCRIPTS, "a.lua", data, [&harness] {
        harness.in_use.names.insert("a.lua");
    });

    AssertError(response, Error::IN_USE);
    zassert_true(harness.FileContent("scripts/a.lua") == old_data, "A file in use is not replaced");
    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_write_whose_in_use_check_throws_is_no_memory_and_leaves_no_session) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(100, 1);
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);
    harness.Put("scripts/a.lua", old_data);

    harness.in_use.throws_bad_alloc = true;
    AssertError(harness.UploadAll(SENSOR_SCRIPTS, "a.lua", data), Error::NO_MEMORY);
    harness.in_use.throws_bad_alloc = false;

    zassert_false(harness.IsStaged());
    zassert_true(harness.FileContent("scripts/a.lua") == old_data);
    AssertError(harness.WriteNext(SENSOR_SCRIPTS, "a.lua", UPLOAD_CHUNK_SIZE, 1, Pattern(10, 1)), Error::BAD_TOKEN);

    // And the group stores a file after it.
    zassert_true(harness.UploadAll(SENSOR_SCRIPTS, "a.lua", data).IsOk());
    zassert_true(harness.FileContent("scripts/a.lua") == data);

    // Before the file is replaced, the check can throw as well: the upload ends, the file stays.
    const Bytes newer = Pattern(2 * UPLOAD_CHUNK_SIZE, 3);

    const auto response = harness.UploadAll(SENSOR_SCRIPTS, "a.lua", newer, [&harness] {
        harness.in_use.throws_bad_alloc = true;
    });
    harness.in_use.throws_bad_alloc = false;

    AssertError(response, Error::NO_MEMORY);
    zassert_false(harness.IsStaged());
    zassert_true(harness.FileContent("scripts/a.lua") == data);
}

ZTEST_F(smp_assets_group, test_write_whose_in_use_check_throws_leaves_the_upload_of_another_client) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());

    harness.in_use.throws_bad_alloc = true;
    AssertError(harness.WriteFirst(SENSOR_SCRIPTS, "b.lua", Pattern(10, 1), 10, Crc(Pattern(10, 1))), Error::NO_MEMORY);
    harness.in_use.throws_bad_alloc = false;

    zassert_true(harness.SendRest(IMAGES, "a.bin", data, started.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());
}

ZTEST_F(smp_assets_group, test_write_that_the_client_leaves_is_abandoned_with_its_staged_file) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(500, 1);
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);
    harness.Put("images/a.bin", old_data);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    zassert_true(harness.IsStaged());

    const atomic_val_t closed_before = atomic_get(&files_closed);
    k_msleep(PAST_TIMEOUT_MS);

    zassert_true(atomic_get(&files_closed) > closed_before, "The idle upload closes its staged file");
    zassert_false(harness.IsStaged());
    zassert_true(harness.FileContent("images/a.bin") == old_data, "The asset is untouched");
    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, started.Value("tok"),
            std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
        ),
        Error::BAD_TOKEN
    );

    // The group takes the next upload.
    zassert_true(harness.UploadAll(IMAGES, "a.bin", data).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_expired_session_does_not_make_another_upload_busy) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const Bytes other = Pattern(2 * UPLOAD_CHUNK_SIZE, 2);

    zassert_true(harness
            .WriteFirst(
                IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
            )
            .IsOk());

    k_msleep(PAST_TIMEOUT_MS);

    zassert_true(harness.UploadAll(IMAGES, "b.bin", other).IsOk());
    zassert_true(harness.FileContent("images/b.bin") == other);
    zassert_false(harness.volume->Exists("images/a.bin"));
}

ZTEST_F(smp_assets_group, test_write_chunks_keep_the_session_alive) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(4 * UPLOAD_CHUNK_SIZE, 1);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());

    // Each pause is shorter than the timeout, together they are longer.
    size_t offset = UPLOAD_CHUNK_SIZE;
    while(offset < data.size()) {
        k_msleep(SESSION_TIMEOUT_MS / 2);

        const auto response = harness.WriteNext(
            IMAGES, "a.bin", offset, started.Value("tok"),
            std::span<const uint8_t>(data).subspan(offset, UPLOAD_CHUNK_SIZE)
        );
        zassert_true(response.IsOk(), "Chunk at %u: %u", static_cast<unsigned>(offset), response.error.value_or(0));
        offset += UPLOAD_CHUNK_SIZE;
    }

    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_the_staged_file_is_not_listed) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 2);

    // A kind on the root of the volume, next to the directory of the staged files, and with the extension
    // of the staged file's name: a staged file anywhere it could be listed would show.
    harness.ReregisterCanScriptsAtRoot();
    harness.Put("keep.lua", Pattern(20, 1));
    harness.Put("other.txt", Pattern(20, 1));

    const auto started = harness.WriteFirst(
        CAN_SCRIPTS, "new.lua", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    zassert_true(harness.IsStaged());

    // The staged file's directory is among the entries of the root the kind lists.
    bool is_staging_directory_there = false;
    for(const auto& entry : harness.RawEntries("")) {
        if(entry.name == ".assets_tmp" && entry.is_directory)
            is_staging_directory_there = true;
    }

    zassert_true(is_staging_directory_there);

    const auto listing = harness.ListAll(CAN_SCRIPTS);

    zassert_equal(listing.files.size(), 1, "Only keep.lua: neither the staged file nor the asset it will be");
    zassert_str_equal(listing.files[0].first.c_str(), "keep.lua");
    zassert_true(harness.ListAll(IMAGES).files.empty());
    zassert_true(harness.ListAll(SENSOR_SCRIPTS).files.empty());
    AssertError(harness.Read(CAN_SCRIPTS, "new.lua", 0), Error::NOT_FOUND);

    // Stored, the asset is listed and the directory still is not.
    zassert_true(harness.SendRest(CAN_SCRIPTS, "new.lua", data, started.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());

    const auto stored = harness.ListAll(CAN_SCRIPTS);
    std::set<std::string> names;
    for(const auto& file : stored.files)
        names.insert(file.first);

    zassert_equal(stored.files.size(), 2);
    zassert_true(names == std::set<std::string>({ "keep.lua", "new.lua" }));
}

ZTEST_F(smp_assets_group, test_write_that_commits_while_the_file_is_being_read_closes_the_download) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(2 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 1);
    const Bytes new_data = Pattern(2 * UPLOAD_CHUNK_SIZE + 300, 2);
    harness.Put("images/a.bin", old_data);

    // A download in the middle of the file.
    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();
    atomic_val_t closed_before = 0;

    const auto response = harness.UploadAll(IMAGES, "a.bin", new_data, [&closed_before] {
        closed_before = atomic_get(&files_closed);
    });

    zassert_true(response.IsOk());

    // The last chunk closed the file of the download and the staged file.
    zassert_equal(atomic_get(&files_closed), closed_before + 2);

    // The download goes on with the new file, which it reads from its start for the CRC. The CRC is
    // the new file's, not that of the chunks the client has got.
    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, new_data.size());

    zassert_true(download.data == Bytes(new_data.begin() + chunk, new_data.end()));
    zassert_equal(download.crc, Crc(new_data));
}

ZTEST_F(smp_assets_group, test_write_forgets_the_checkpoint_of_the_file_it_replaces) {
    Harness& harness = *fixture->harness;
    const size_t size = 2 * UPLOAD_CHUNK_SIZE;
    const Bytes old_data = Pattern(size, 1);
    const Bytes new_data = Pattern(size, 2);
    harness.Put("images/a.bin", old_data);

    // A download in the middle of the old file: closing it for the commit leaves its checkpoint.
    const size_t chunk = Harness::DataOf(harness.Read(IMAGES, "a.bin", 0)).size();
    zassert_true(chunk < size);

    // The new file has the same size, so only the checkpoint's forgetting keeps its CRC from the old one.
    zassert_true(harness.UploadAll(IMAGES, "a.bin", new_data).IsOk());

    const auto download = harness.DownloadFrom(IMAGES, "a.bin", chunk, size);

    zassert_true(download.data == Bytes(new_data.begin() + chunk, new_data.end()));
    zassert_equal(download.crc, Crc(new_data), "The CRC of the old content is not kept");
}

ZTEST_F(smp_assets_group, test_write_last_chunk_sent_again_gets_the_answer_again) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    const Bytes other = Pattern(data.size(), 2);
    const auto last = std::span<const uint8_t>(data).subspan(2 * UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, UPLOAD_CHUNK_SIZE).IsOk());

    // The file is stored. Someone changes it, to see whether the chunk that is sent again writes.
    harness.Put("images/a.bin", other);

    const auto again = harness.WriteNext(IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, token, last);

    zassert_true(again.IsOk());
    zassert_equal(again.Value("off"), data.size());
    zassert_false(again.Has("tok"));
    zassert_true(harness.FileContent("images/a.bin") == other, "Nothing was written");
    zassert_false(harness.IsStaged());

    // Also while another client's upload is open.
    const auto next = harness.WriteFirst(
        IMAGES, "b.bin", std::span<const uint8_t>(other).first(UPLOAD_CHUNK_SIZE), other.size(), Crc(other)
    );
    zassert_true(next.IsOk());

    const auto while_busy = harness.WriteNext(IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, token, last);

    zassert_true(while_busy.IsOk());
    zassert_equal(while_busy.Value("off"), data.size());

    // Not an earlier chunk, not another content, not another token or name.
    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token,
            std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
        ),
        Error::BAD_TOKEN
    );
    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, token,
            std::span<const uint8_t>(other).subspan(2 * UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
        ),
        Error::BAD_TOKEN
    );
    AssertError(harness.WriteNext(IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, token + 1, last), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(IMAGES, "b.bin", 2 * UPLOAD_CHUNK_SIZE, token, last), Error::BAD_TOKEN);
    AssertError(harness.WriteNext(IMAGES, "a.bin", 2 * UPLOAD_CHUNK_SIZE, std::nullopt, last), Error::BAD_TOKEN);

    // The other upload is as it was.
    zassert_true(harness.SendRest(IMAGES, "b.bin", other, next.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());
    zassert_true(harness.FileContent("images/b.bin") == other);
}

ZTEST_F(smp_assets_group, test_write_last_chunk_sent_again_late_is_bad_token) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(2 * UPLOAD_CHUNK_SIZE, 1);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");

    zassert_true(harness.SendRest(IMAGES, "a.bin", data, token, UPLOAD_CHUNK_SIZE).IsOk());

    k_msleep(PAST_TIMEOUT_MS);

    AssertError(
        harness.WriteNext(
            IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token,
            std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
        ),
        Error::BAD_TOKEN
    );
}

ZTEST_F(smp_assets_group, test_write_that_cannot_write_is_no_space_or_io_error_and_ends_the_upload) {
    Harness& harness = *fixture->harness;
    harness.ReregisterImages(20000);
    const Bytes old_data = Pattern(100, 1);
    const Bytes data = Pattern(9000, 5);
    harness.Put("images/big.bin", old_data);

    struct Case {
        atomic_val_t fault;
        Error expected;
    };

    const std::array<Case, 3> cases = { {
        { -ENOSPC, Error::NO_SPACE },
        { WRITE_FAULT_SHORT, Error::NO_SPACE },
        { -EIO, Error::IO_ERROR },
    } };

    for(const Case& test_case : cases) {
        const auto started = harness.WriteFirst(
            IMAGES, "big.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
        );
        zassert_true(started.IsOk());
        const uint32_t token = started.Value("tok");

        // The chunks fill the buffer of the staged file with the eighth, which writes it out.
        for(size_t chunk = 1; chunk < 7; chunk++) {
            zassert_true(harness
                    .WriteNext(
                        IMAGES, "big.bin", chunk * UPLOAD_CHUNK_SIZE, token,
                        std::span<const uint8_t>(data).subspan(chunk * UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
                    )
                    .IsOk());
        }

        atomic_set(&write_fault, test_case.fault);
        const auto failed = harness.WriteNext(
            IMAGES, "big.bin", 7 * UPLOAD_CHUNK_SIZE, token,
            std::span<const uint8_t>(data).subspan(7 * UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE)
        );
        atomic_set(&write_fault, 0);

        AssertError(failed, test_case.expected);
        zassert_false(harness.IsStaged());
        zassert_true(harness.FileContent("images/big.bin") == old_data, "The asset is untouched");
        AssertError(
            harness.WriteNext(
                IMAGES, "big.bin", 8 * UPLOAD_CHUNK_SIZE, token,
                std::span<const uint8_t>(data).subspan(8 * UPLOAD_CHUNK_SIZE, 100)
            ),
            Error::BAD_TOKEN
        );
    }
}

ZTEST_F(smp_assets_group, test_write_that_cannot_store_the_file_is_no_space_or_io_error_and_keeps_the_old_one) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(100, 1);
    const Bytes data = Pattern(300, 5);
    harness.Put("images/a.bin", old_data);

    for(const atomic_val_t fault : { static_cast<atomic_val_t>(-ENOSPC), static_cast<atomic_val_t>(-EIO) }) {
        // The buffer of the staged file is written out when the file is stored.
        atomic_set(&write_fault, fault);
        const auto failed = harness.WriteFirst(IMAGES, "a.bin", data, data.size(), Crc(data));
        atomic_set(&write_fault, 0);

        AssertError(failed, fault == -ENOSPC ? Error::NO_SPACE : Error::IO_ERROR);
        zassert_false(harness.IsStaged());
        zassert_true(harness.FileContent("images/a.bin") == old_data);
    }

    // The upload that follows is stored.
    zassert_true(harness.WriteFirst(IMAGES, "a.bin", data, data.size(), Crc(data)).IsOk());
    zassert_true(harness.FileContent("images/a.bin") == data);
}

ZTEST_F(smp_assets_group, test_write_larger_than_the_volume_is_no_space) {
    Harness& harness = *fixture->harness;
    const Bytes old_data = Pattern(100, 1);
    harness.ReregisterImages(8 * 1024 * 1024);
    harness.Put("images/a.bin", old_data);

    // More than the 1 MiB the volume has, declared in the first chunk.
    const Bytes first = Pattern(100, 2);

    AssertError(harness.WriteFirst(IMAGES, "a.bin", first, 4 * 1024 * 1024, 0), Error::NO_SPACE);
    zassert_false(harness.IsStaged());
    zassert_true(harness.FileContent("images/a.bin") == old_data, "The old file is still there");
    AssertError(harness.WriteNext(IMAGES, "a.bin", 100, 1, first), Error::BAD_TOKEN);
}

ZTEST_F(smp_assets_group, test_write_into_a_nearly_full_volume_is_no_space) {
    Harness& harness = *fixture->harness;
    constexpr uint64_t NEARLY_FULL = 24ULL * 1024;
    const Bytes old_data = Pattern(100, 1);
    harness.ReregisterImages(1000000);
    harness.Put("images/a.bin", old_data);

    // Fill the volume up to a few blocks. The harness empties it at the end of the test, whatever happens.
    const Bytes filler = Pattern(8192, 9);
    zassert_true(harness.volume->CreateDirectory("filler"));

    for(size_t i = 0; i < 300; i++) {
        if(harness.volume->GetTotalSpace() - harness.volume->GetUsedSpace() < NEARLY_FULL)
            break;

        if(!harness.volume->WriteFile("filler/f" + std::to_string(i) + ".bin", filler.data(), filler.size()))
            break;
    }

    zassert_true(
        harness.volume->GetTotalSpace() - harness.volume->GetUsedSpace() < NEARLY_FULL, "The volume has room left"
    );

    const Bytes first = Pattern(100, 2);

    AssertError(harness.WriteFirst(IMAGES, "a.bin", first, 64 * 1024, 0), Error::NO_SPACE);
    zassert_false(harness.IsStaged());
    zassert_true(harness.FileContent("images/a.bin") == old_data);

    zassert_true(harness.volume->DeleteRecursive("filler"));
}

ZTEST_F(smp_assets_group, test_write_stops_when_the_volume_goes_away) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");
    const auto second = std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE);

    harness.images_volume->available = false;
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, second), Error::UNAVAILABLE);
    harness.images_volume->available = true;

    // The upload is over, and the volume takes the next.
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, second), Error::BAD_TOKEN);
    harness.volume->DeleteFile(STAGED_PATH);
    zassert_true(harness.UploadAll(IMAGES, "a.bin", data).IsOk());
}

ZTEST_F(smp_assets_group, test_write_stops_when_its_kind_is_unregistered) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());
    const uint32_t token = started.Value("tok");
    const auto second = std::span<const uint8_t>(data).subspan(UPLOAD_CHUNK_SIZE, UPLOAD_CHUNK_SIZE);

    harness.registry->Unregister(AssetKind::IMAGES);
    AssertError(harness.WriteNext(IMAGES, "a.bin", UPLOAD_CHUNK_SIZE, token, second), Error::UNKNOWN_KIND);
    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_a_destroyed_group_aborts_its_upload) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);

    zassert_true(harness
            .WriteFirst(
                IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
            )
            .IsOk());
    zassert_true(harness.IsStaged());

    const atomic_val_t closed_before = atomic_get(&files_closed);
    harness.group.reset();

    zassert_equal(atomic_get(&files_closed), closed_before + 1);
    zassert_false(harness.IsStaged());
    zassert_false(harness.volume->Exists("images/a.bin"));

    // Nothing of the group runs later.
    k_msleep(PAST_TIMEOUT_MS);
    zassert_equal(atomic_get(&files_closed), closed_before + 1);
}

ZTEST_F(smp_assets_group, test_initialize_deletes_a_staged_file_a_power_loss_left) {
    Harness& harness = *fixture->harness;
    harness.Put(".assets_tmp/upload.part", Pattern(1000, 1));
    zassert_true(harness.IsStaged());

    harness.StartGroup();

    zassert_false(harness.IsStaged());
    zassert_true(harness.Kinds().IsOk());
}

ZTEST_F(smp_assets_group, test_a_request_to_write_without_its_keys_is_invalid) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(10, 1);
    const std::span<const uint8_t> chunk(data);
    const uint8_t write_command = std::to_underlying(Command::WRITE);

    const auto client_write = [&harness](const Bytes& body) {
        return harness.client.Write(SmpGroupId::ASSETS, write_command, body);
    };

    // The first chunk needs `len` and `crc`.
    zassert_equal(
        client_write(
            CborMap()
                .Put("kind", IMAGES)
                .PutText("name", "a.bin")
                .Put("off", 0)
                .Put("crc", 0)
                .Put("data", chunk)
                .Build()
        )
            .Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_write(
            CborMap()
                .Put("kind", IMAGES)
                .PutText("name", "a.bin")
                .Put("off", 0)
                .Put("len", 10)
                .Put("data", chunk)
                .Build()
        )
            .Value("rc"),
        MGMT_ERR_EINVAL
    );

    // Every chunk needs the rest.
    zassert_equal(
        client_write(
            CborMap().PutText("name", "a.bin").Put("off", 0).Put("len", 10).Put("crc", 0).Put("data", chunk).Build()
        )
            .Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_write(
            CborMap().Put("kind", IMAGES).Put("off", 0).Put("len", 10).Put("crc", 0).Put("data", chunk).Build()
        )
            .Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_write(
            CborMap()
                .Put("kind", IMAGES)
                .PutText("name", "a.bin")
                .Put("len", 10)
                .Put("crc", 0)
                .Put("data", chunk)
                .Build()
        )
            .Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        client_write(CborMap().Put("kind", IMAGES).PutText("name", "a.bin").Put("off", 10).Put("tok", 1).Build())
            .Value("rc"),
        MGMT_ERR_EINVAL
    );

    zassert_false(harness.IsStaged());
}

ZTEST_F(smp_assets_group, test_write_deletes_a_stale_staged_file_before_it_looks_at_the_room) {
    Harness& harness = *fixture->harness;
    constexpr uint32_t LENGTH = 512 * 1024;
    harness.ReregisterImages(1000000);

    // What an upload left when its card was pulled in the middle of it: most of the volume is the staged
    // file, and nothing is open.
    const Bytes block = Pattern(8192, 3);
    zassert_true(harness.volume->CreateDirectory(".assets_tmp"));
    for(size_t i = 0; i < 100; i++)
        zassert_true(harness.volume->WriteFile(STAGED_PATH, block.data(), block.size(), true));

    zassert_true(
        harness.volume->GetTotalSpace() - harness.volume->GetUsedSpace() < LENGTH, "The stale file takes the room"
    );

    const Bytes first = Pattern(UPLOAD_CHUNK_SIZE, 4);
    const auto started = harness.WriteFirst(IMAGES, "big.bin", first, LENGTH, 0);

    zassert_true(started.IsOk(), "Refused with %u", started.error.value_or(0));
    zassert_true(harness.IsStaged());
    zassert_true(
        harness.volume->GetFileSize(STAGED_PATH).value_or(UINT32_MAX) < block.size(), "The staged file is the upload's"
    );
}

ZTEST_F(smp_assets_group, test_write_is_busy_before_it_looks_at_the_room) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    harness.ReregisterImages(8 * 1024 * 1024);

    const auto started = harness.WriteFirst(
        IMAGES, "a.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
    );
    zassert_true(started.IsOk());

    // More than the volume holds, from another client.
    AssertError(harness.WriteFirst(IMAGES, "b.bin", Pattern(100, 2), 4 * 1024 * 1024, 0), Error::BUSY);

    // The upload that is open is as it was, and with none open the same request is out of room.
    zassert_true(harness.SendRest(IMAGES, "a.bin", data, started.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());
    AssertError(harness.WriteFirst(IMAGES, "b.bin", Pattern(100, 2), 4 * 1024 * 1024, 0), Error::NO_SPACE);
}

ZTEST_F(smp_assets_group, test_write_one_chunk_file_sent_again_gets_its_token_again) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 1);
    const Bytes other = Pattern(300, 2);
    const Bytes busy = Pattern(3 * UPLOAD_CHUNK_SIZE, 3);

    const auto stored = harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data));
    zassert_true(stored.IsOk());
    const uint32_t token = stored.Value("tok");

    // The file changes at the same size, to see whether the request sent again writes, and another
    // client opens an upload.
    harness.Put("images/one.bin", other);

    const auto started = harness.WriteFirst(
        IMAGES, "b.bin", std::span<const uint8_t>(busy).first(UPLOAD_CHUNK_SIZE), busy.size(), Crc(busy)
    );
    zassert_true(started.IsOk());

    // The answer got lost: the same request, without a token. It is no stranger to be told to wait for.
    const auto again = harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data));

    zassert_true(again.IsOk(), "Refused with %u", again.error.value_or(0));
    zassert_equal(again.Value("off"), data.size());
    zassert_equal(again.Value("tok"), token);
    zassert_true(harness.FileContent("images/one.bin") == other, "Nothing was written");

    // Another content or another declared CRC is another upload.
    AssertError(harness.WriteFirst(IMAGES, "one.bin", other, other.size(), Crc(other)), Error::BUSY);
    AssertError(harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data) + 1), Error::BUSY);

    // The upload that is open is as it was.
    zassert_true(harness.SendRest(IMAGES, "b.bin", busy, started.Value("tok"), UPLOAD_CHUNK_SIZE).IsOk());
}

ZTEST_F(smp_assets_group, test_write_empty_file_sent_again_gets_its_token_again) {
    Harness& harness = *fixture->harness;

    const auto stored = harness.WriteFirst(IMAGES, "e.bin", {}, 0, 0);
    zassert_true(stored.IsOk());
    const uint32_t token = stored.Value("tok");

    const auto again = harness.WriteFirst(IMAGES, "e.bin", {}, 0, 0);

    zassert_true(again.IsOk());
    zassert_equal(again.Value("off"), 0);
    zassert_equal(again.Value("tok"), token);
    zassert_equal(harness.volume->GetFileSize("images/e.bin").value_or(99), 0);
}

ZTEST_F(smp_assets_group, test_write_one_chunk_file_sent_again_after_the_file_is_gone_is_stored_again) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(300, 1);

    zassert_true(harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data)).IsOk());

    // Deleted through the group, and the request comes again.
    zassert_true(harness.Delete(IMAGES, "one.bin").IsOk());
    zassert_false(harness.volume->Exists("images/one.bin"));
    zassert_true(harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data)).IsOk());
    zassert_true(harness.FileContent("images/one.bin") == data);

    // Deleted by someone else.
    zassert_true(harness.volume->DeleteFile("images/one.bin"));
    zassert_true(harness.WriteFirst(IMAGES, "one.bin", data, data.size(), Crc(data)).IsOk());
    zassert_true(harness.FileContent("images/one.bin") == data);
}

ZTEST_F(smp_assets_group, test_a_destroyed_group_with_a_download_and_an_upload_goes_quiet) {
    Harness& harness = *fixture->harness;
    const Bytes data = Pattern(3 * UPLOAD_CHUNK_SIZE, 1);
    harness.Put("images/a.bin", Pattern(3 * CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 2));

    zassert_true(harness.Read(IMAGES, "a.bin", 0).IsOk());
    zassert_true(harness
            .WriteFirst(
                IMAGES, "b.bin", std::span<const uint8_t>(data).first(UPLOAD_CHUNK_SIZE), data.size(), Crc(data)
            )
            .IsOk());

    // The idle task is armed for both.
    const atomic_val_t closed_before = atomic_get(&files_closed);
    harness.group.reset();

    zassert_equal(atomic_get(&files_closed), closed_before + 2);
    zassert_false(harness.IsStaged());

    k_msleep(PAST_TIMEOUT_MS);
    zassert_equal(atomic_get(&files_closed), closed_before + 2);
}

// ======== Dispatch ========

ZTEST_F(smp_assets_group, test_unregistered_group_is_not_supported) {
    Harness& harness = *fixture->harness;
    harness.group->Unregister();

    zassert_equal(harness.Kinds().Value("rc"), MGMT_ERR_ENOTSUP, "As firmware without the group answers");
    zassert_equal(harness.List(IMAGES).Value("rc"), MGMT_ERR_ENOTSUP);
    zassert_equal(harness.Read(IMAGES, "a.bin", 0).Value("rc"), MGMT_ERR_ENOTSUP);
    zassert_equal(harness.Delete(IMAGES, "a.bin").Value("rc"), MGMT_ERR_ENOTSUP);
}

ZTEST_F(smp_assets_group, test_a_group_that_is_served_again_answers) {
    Harness& harness = *fixture->harness;
    harness.group->Unregister();
    harness.group->Register();

    zassert_true(harness.Kinds().IsOk());
}
