#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_store.h"
#include "subsys/fs/services/i_fs_service.h"

#include "assets_test_support.h"

// This file builds into two suites: assets, on the littlefs volume of the board's devicetree, and
// assets_fat, on a FAT volume. FAT renames, seeks and reports a full volume differently, which the
// store has to absorb, so nothing below may depend on the file system under it. A directory lists
// in the file system's own order: tests that page through one compare against the listing they read
// first.

using eerie_leap::subsys::assets::AssetEntry;
using eerie_leap::subsys::assets::AssetReader;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::AssetWriter;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::IFsService;

using assets_test::GetFsService;
using assets_test::MakeBlob;
using assets_test::UnavailableFsService;

namespace asset_store_test {

/** A read or a write that went through fs_read() or fs_write(). */
struct FsCall {
    /// Where the file position was when the call was made.
    off_t position;
    /// The bytes the call asked for.
    size_t size;
    /// What the call returned.
    ssize_t result;
};

/**
 * What the wrapped fs_read(), fs_write() and fs_seek() saw while the recorder was on. It is off
 * unless a test starts it, so that nothing else notices the wrappers.
 *
 * The wrappers (below) take effect through the link flags -Wl,--wrap=fs_read,--wrap=fs_write,
 * --wrap=fs_seek, which the CMakeLists.txt of both suites that build this file carry.
 */
struct FsRecorder {
    bool on = false;
    std::vector<FsCall> reads;
    std::vector<FsCall> writes;
    size_t seeks = 0;

    void Start() {
        Reset();
        // Room for the calls of a test, so that fs_read() and fs_write() allocate nothing.
        reads.reserve(64);
        writes.reserve(64);
        on = true;
    }

    void Stop() {
        on = false;
    }

    void Reset() {
        on = false;
        reads.clear();
        writes.clear();
        seeks = 0;
    }
};

static FsRecorder& Recorder() {
    static FsRecorder recorder;

    return recorder;
}

} // namespace asset_store_test

using asset_store_test::Recorder;

namespace {

constexpr std::string_view ASSETS_DIR = "test_assets";
constexpr std::array<std::string_view, 1> BIN_EXTENSIONS = { ".bin" };
constexpr size_t MAX_NAME_LENGTH = CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH;

// Appends that cross the 4096 byte buffer of a writer in every way: well short of it, one byte
// short, exactly, one byte over, and across more than one.
constexpr std::array<size_t, 6> ODD_PIECES = { 1, 4095, 4096, 4097, 3, 1000 };

// Sizes of the requests a reader is read with.
constexpr std::array<size_t, 9> READ_PIECES = { 1, 13, 1000, 4095, 4096, 4097, 9999, 10000, 20000 };

// Sizes around the buffers of the reader and the writer.
constexpr std::array<size_t, 7> EDGE_SIZES = { 0, 1, 4095, 4096, 4097, 8192, 8193 };

// Sizes of the files whose reads and writes the block tests follow.
constexpr std::array<size_t, 8> BLOCK_SIZES = { 0, 1, 4095, 4096, 4097, 8192, 8193, 10000 };

// The file system underneath sees reads and writes of whole sectors at sector boundaries only because
// the blocks are this size; the block tests below hold the reader and the writer to it.
static_assert(AssetReader::BUFFER_SIZE == 4096);
static_assert(AssetWriter::BUFFER_SIZE == 4096);

void ResetFs(void* = nullptr) {
    Recorder().Reset();
    GetFsService()->DeleteRecursive();
}

AssetStore MakeStore(std::string_view directory = ASSETS_DIR) {
    return { GetFsService(), directory };
}

bool WriteBlob(std::string_view path, size_t size, uint8_t seed = 0) {
    const auto blob = MakeBlob(size, seed);

    return GetFsService()->WriteFile(path, blob.data(), blob.size());
}

bool StagedFileExists() {
    return GetFsService()->Exists(AssetStore::STAGING_PATH);
}

// Writes a file where a power loss during an upload would have left one.
void LeaveStagedFile(size_t size) {
    zassert_true(GetFsService()->CreateDirectory(AssetStore::STAGING_DIRECTORY));
    zassert_true(WriteBlob(AssetStore::STAGING_PATH, size, 99));
}

// Appends the blob in pieces of the given sizes, one after the other, cycling through them.
int AppendInPieces(AssetWriter& writer, const std::vector<uint8_t>& blob, std::span<const size_t> pieces) {
    size_t offset = 0;

    for(size_t i = 0; offset < blob.size(); ++i) {
        const size_t length = std::min(pieces[i % pieces.size()], blob.size() - offset);
        const int rc = writer.Append(std::span<const uint8_t>(blob.data() + offset, length));
        if(rc != 0)
            return rc;

        offset += length;
    }

    return 0;
}

// Appends blocks until the volume refuses one. Returns the error, or 0 when the volume never filled.
int AppendUntilFull(AssetWriter& writer, size_t& appended) {
    // The volumes are 1 MiB; twice that bounds a volume that never fills.
    constexpr size_t MAX_BLOCKS = 512;
    const auto block = MakeBlob(AssetWriter::BUFFER_SIZE, 7);

    appended = 0;

    for(size_t i = 0; i < MAX_BLOCKS; ++i) {
        const int rc = writer.Append(block);
        if(rc != 0)
            return rc;

        appended += block.size();
    }

    return 0;
}

// Reads the whole asset with requests of the given size.
std::vector<uint8_t> ReadAll(AssetReader& reader, size_t request_size) {
    std::vector<uint8_t> content;
    std::vector<uint8_t> buffer(request_size);

    while(true) {
        const ssize_t bytes_read = reader.Read(content.size(), buffer);
        zassert_true(bytes_read >= 0, "Read failed: %d", static_cast<int>(bytes_read));

        if(bytes_read == 0)
            break;

        content.insert(content.end(), buffer.begin(), buffer.begin() + bytes_read);
        zassert_true(content.size() <= reader.GetSize(), "Read returned more than the file holds");
    }

    return content;
}

// For blobs that may be empty, whose data() is no pointer to hand to memcmp().
void AssertSameContent(std::span<const uint8_t> actual, std::span<const uint8_t> expected) {
    zassert_equal(
        actual.size(), expected.size(), "%d bytes instead of %d", static_cast<int>(actual.size()),
        static_cast<int>(expected.size())
    );
    zassert_true(std::equal(actual.begin(), actual.end(), expected.begin()), "The content differs");
}

// Checks the calls that moved a file of the given size a block at a time: one per block, at the start
// of the block, as long as the file allows and complete.
void AssertBlockCalls(const std::vector<asset_store_test::FsCall>& calls, size_t size, size_t block_size) {
    const size_t blocks = (size + block_size - 1) / block_size;

    zassert_equal(
        calls.size(), blocks, "%d calls for %d bytes", static_cast<int>(calls.size()), static_cast<int>(size)
    );

    for(size_t i = 0; i < calls.size(); ++i) {
        const size_t start = i * block_size;
        const size_t length = std::min(block_size, size - start);

        zassert_equal(
            static_cast<size_t>(calls[i].position), start, "Call %d of %d bytes started at %d", static_cast<int>(i),
            static_cast<int>(size), static_cast<int>(calls[i].position)
        );
        zassert_equal(
            calls[i].size, length, "Call %d of %d bytes asked for %d", static_cast<int>(i), static_cast<int>(size),
            static_cast<int>(calls[i].size)
        );
        zassert_equal(
            calls[i].result, static_cast<ssize_t>(length), "Call %d of %d bytes moved %d", static_cast<int>(i),
            static_cast<int>(size), static_cast<int>(calls[i].result)
        );
    }
}

struct Listing {
    int rc = 0;
    std::vector<AssetEntry> entries;
    size_t scanned = 0;
    size_t count = 0;
};

Listing ListPage(const AssetStore& store, size_t first, size_t max_entries) {
    Listing listing;
    listing.rc = store.List(first, max_entries, BIN_EXTENSIONS, listing.entries, listing.scanned, listing.count);

    return listing;
}

const AssetEntry& EntryNamed(const std::vector<AssetEntry>& entries, std::string_view name) {
    const auto entry = std::ranges::find(entries, name, &AssetEntry::name);
    zassert_true(entry != entries.end(), "No entry named %.*s", static_cast<int>(name.size()), name.data());

    return *entry;
}

// The position of an entry in the directory, as the file system service reads it.
size_t PositionOf(std::string_view directory, std::string_view name) {
    std::vector<DirectoryEntry> raw;
    size_t count = 0;

    zassert_ok(GetFsService()->ListDirectory(directory, 0, 1000, raw, count));

    const auto entry = std::ranges::find(raw, name, &DirectoryEntry::name);
    zassert_true(entry != raw.end(), "No entry named %.*s", static_cast<int>(name.size()), name.data());

    return static_cast<size_t>(entry - raw.begin());
}

// Forwards to another service, except where a test has it misbehave.
class ScriptedFsService : public IFsService {
private:
    std::shared_ptr<IFsService> inner_;

public:
    // The volume is reported unavailable, while everything underneath still works: an SD card
    // pulled while its monitor is off.
    bool unavailable = false;
    // Added to the size GetFileSize() reports, as if the file had changed since.
    int64_t size_delta = 0;
    // The size GetFileSize() reports for every path, whatever it holds: as littlefs does for a
    // directory, whose size it leaves undefined.
    std::optional<size_t> forced_file_size;
    // What Rename() returns instead of renaming; 0 renames.
    int rename_result = 0;
    // CreateDirectory() fails for this path.
    std::string failing_directory;

    explicit ScriptedFsService(std::shared_ptr<IFsService> inner) :
        inner_(std::move(inner)) {}

    bool Initialize() override {
        return inner_->Initialize();
    }
    bool IsAvailable() const override {
        return !unavailable && inner_->IsAvailable();
    }
    bool WriteFile(std::string_view path, const void* data, size_t size, bool append) override {
        return inner_->WriteFile(path, data, size, append);
    }
    bool ReadFile(std::string_view path, void* data, size_t size, size_t& out_len) override {
        return inner_->ReadFile(path, data, size, out_len);
    }
    int OpenFile(std::string_view path, fs_mode_t flags, fs_file_t* file) override {
        return inner_->OpenFile(path, flags, file);
    }
    bool CreateDirectory(std::string_view path) override {
        if(!failing_directory.empty() && path == failing_directory)
            return false;

        return inner_->CreateDirectory(path);
    }
    bool Exists(std::string_view path) const override {
        return inner_->Exists(path);
    }
    bool DeleteFile(std::string_view path) override {
        return inner_->DeleteFile(path);
    }
    bool DeleteRecursive(std::string_view path) override {
        return inner_->DeleteRecursive(path);
    }
    std::vector<std::string> ListFiles(std::string_view path) const override {
        return inner_->ListFiles(path);
    }
    int ListDirectory(
        std::string_view path,
        size_t first,
        size_t max_entries,
        std::vector<DirectoryEntry>& entries,
        size_t& count
    ) const override {
        return inner_->ListDirectory(path, first, max_entries, entries, count);
    }
    std::optional<size_t> GetFileSize(std::string_view path) const override {
        if(forced_file_size.has_value())
            return forced_file_size;

        auto size = inner_->GetFileSize(path);
        if(size.has_value())
            size = static_cast<size_t>(static_cast<int64_t>(*size) + size_delta);

        return size;
    }
    uint64_t GetTotalSpace() const override {
        return inner_->GetTotalSpace();
    }
    uint64_t GetUsedSpace() const override {
        return inner_->GetUsedSpace();
    }
    bool Format() override {
        return inner_->Format();
    }
    int Rename(std::string_view from, std::string_view to) override {
        return rename_result != 0 ? rename_result : inner_->Rename(from, to);
    }
};

} // namespace

// The linker routes every call of fs_read(), fs_write() and fs_seek() here (--wrap). Each passes the
// call on, and tells the recorder about it while the recorder is on.
extern "C" {

ssize_t __real_fs_read(struct fs_file_t* zfp, void* ptr, size_t size);
ssize_t __real_fs_write(struct fs_file_t* zfp, const void* ptr, size_t size);
int __real_fs_seek(struct fs_file_t* zfp, off_t offset, int whence);

ssize_t __wrap_fs_read(struct fs_file_t* zfp, void* ptr, size_t size) {
    auto& recorder = asset_store_test::Recorder();

    if(!recorder.on)
        return __real_fs_read(zfp, ptr, size);

    const off_t position = fs_tell(zfp);
    const ssize_t rc = __real_fs_read(zfp, ptr, size);

    recorder.reads.push_back({ position, size, rc });

    return rc;
}

ssize_t __wrap_fs_write(struct fs_file_t* zfp, const void* ptr, size_t size) {
    auto& recorder = asset_store_test::Recorder();

    if(!recorder.on)
        return __real_fs_write(zfp, ptr, size);

    const off_t position = fs_tell(zfp);
    const ssize_t rc = __real_fs_write(zfp, ptr, size);

    recorder.writes.push_back({ position, size, rc });

    return rc;
}

int __wrap_fs_seek(struct fs_file_t* zfp, off_t offset, int whence) {
    auto& recorder = asset_store_test::Recorder();

    if(recorder.on)
        ++recorder.seeks;

    return __real_fs_seek(zfp, offset, whence);
}

} // extern "C"

ZTEST_SUITE(asset_store, NULL, NULL, ResetFs, ResetFs, NULL);

// ======== Whole files ========

ZTEST(asset_store, test_construction_creates_assets_dir) {
    zassert_false(GetFsService()->Exists(ASSETS_DIR));

    auto store = MakeStore();

    zassert_true(GetFsService()->Exists(ASSETS_DIR));
    zassert_true(store.GetDirectory() == ASSETS_DIR);
}

ZTEST(asset_store, test_construction_keeps_existing_assets_dir) {
    auto blob = MakeBlob(16);

    {
        auto store = MakeStore();
        zassert_true(store.Save("asset.bin", blob));
    }

    auto store = MakeStore();

    zassert_true(store.Exists("asset.bin"));
}

ZTEST(asset_store, test_save_and_load_roundtrip) {
    auto store = MakeStore();
    auto blob = MakeBlob(512, 7);

    zassert_true(store.Save("image.bin", blob));

    auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_save_writes_below_assets_dir) {
    auto store = MakeStore();
    auto blob = MakeBlob(8);

    zassert_true(store.Save("image.bin", blob));

    zassert_true(GetFsService()->Exists(std::string(ASSETS_DIR) + "/image.bin"));
    zassert_false(GetFsService()->Exists("image.bin"));
}

ZTEST(asset_store, test_save_overwrites_previous_content) {
    auto store = MakeStore();

    zassert_true(store.Save("image.bin", MakeBlob(256, 1)));

    auto smaller_blob = MakeBlob(32, 2);
    zassert_true(store.Save("image.bin", smaller_blob));

    auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), smaller_blob.size());
    zassert_mem_equal(loaded.data(), smaller_blob.data(), smaller_blob.size());
}

ZTEST(asset_store, test_save_empty_data_produces_empty_load) {
    auto store = MakeStore();

    zassert_true(store.Save("empty.bin", std::span<const uint8_t>{}));
    zassert_true(store.Exists("empty.bin"));
    zassert_true(store.Load("empty.bin").empty());
}

ZTEST(asset_store, test_save_creates_the_directory_when_it_is_missing) {
    auto store = MakeStore();
    auto blob = MakeBlob(32, 4);

    zassert_true(GetFsService()->DeleteFile(ASSETS_DIR));
    zassert_false(GetFsService()->Exists(ASSETS_DIR));

    zassert_true(store.Save("asset.bin", blob));
    zassert_true(GetFsService()->Exists(std::string(ASSETS_DIR) + "/asset.bin"));
}

ZTEST(asset_store, test_load_missing_asset_returns_empty) {
    auto store = MakeStore();

    zassert_true(store.Load("missing.bin").empty());
}

ZTEST(asset_store, test_load_of_a_directory_returns_empty) {
    auto store = MakeStore();

    zassert_true(GetFsService()->CreateDirectory(std::string(ASSETS_DIR) + "/folder"));

    zassert_true(store.Load("folder").empty());
}

// The size of a directory is undefined on littlefs, so it can be anything, and a size that no buffer
// can hold must give an empty result, not an exception out of the vector.
ZTEST(asset_store, test_load_of_a_size_no_buffer_can_hold_returns_empty) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);
    const auto blob = MakeBlob(100, 6);

    zassert_true(store.Save("image.bin", blob));
    zassert_true(GetFsService()->CreateDirectory(std::string(ASSETS_DIR) + "/folder"));

    // More than a vector can hold: std::length_error.
    scripted->forced_file_size = std::numeric_limits<size_t>::max() / 2;
    zassert_true(store.Load("image.bin").empty());
    zassert_true(store.Load("folder").empty());

    // More than the heap holds: std::bad_alloc.
    scripted->forced_file_size = size_t{ 1 } << 30;
    zassert_true(store.Load("image.bin").empty());
    zassert_true(store.Load("folder").empty());

    // The store is none the worse for it.
    scripted->forced_file_size.reset();

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

// The size is read first and the content after, so a file that changed in between must not come
// back as a mixture of the two.
ZTEST(asset_store, test_load_of_a_file_whose_size_changed_returns_empty) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);
    auto blob = MakeBlob(1000, 4);

    zassert_true(store.Save("image.bin", blob));

    auto loaded = store.Load("image.bin");
    zassert_equal(loaded.size(), blob.size(), "The scripted service must not change what it forwards");
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());

    // The file grew after its size was read: the read finds one byte more than the size says.
    scripted->size_delta = -1;
    zassert_true(store.Load("image.bin").empty());

    // The file shrank: the read finds less than the size says.
    scripted->size_delta = 1;
    zassert_true(store.Load("image.bin").empty());
}

ZTEST(asset_store, test_exists_reflects_save_and_delete) {
    auto store = MakeStore();

    zassert_false(store.Exists("image.bin"));

    zassert_true(store.Save("image.bin", MakeBlob(16)));
    zassert_true(store.Exists("image.bin"));

    zassert_true(store.Delete("image.bin"));
    zassert_false(store.Exists("image.bin"));
    zassert_true(store.Load("image.bin").empty());
}

ZTEST(asset_store, test_delete_missing_asset_fails) {
    auto store = MakeStore();

    zassert_false(store.Delete("missing.bin"));
}

ZTEST(asset_store, test_get_size_reports_the_size_of_a_file) {
    auto store = MakeStore();

    zassert_false(store.GetSize("image.bin").has_value());

    zassert_true(store.Save("image.bin", MakeBlob(300)));
    zassert_true(store.GetSize("image.bin").has_value());
    zassert_equal(store.GetSize("image.bin").value(), 300);

    zassert_true(store.Save("empty.bin", std::span<const uint8_t>{}));
    zassert_true(store.GetSize("empty.bin").has_value());
    zassert_equal(store.GetSize("empty.bin").value(), 0);
}

// A directory has no size to serve: littlefs leaves it undefined, and FAT says zero.
ZTEST(asset_store, test_get_size_of_a_directory_is_nullopt) {
    auto store = MakeStore();

    zassert_true(GetFsService()->CreateDirectory(std::string(ASSETS_DIR) + "/folder"));

    zassert_true(store.Exists("folder"));
    zassert_false(store.GetSize("folder").has_value());
}

ZTEST(asset_store, test_stores_with_different_dirs_are_isolated) {
    auto ui_assets = MakeStore("ui_assets");
    auto fw_assets = MakeStore("fw_assets");

    auto ui_blob = MakeBlob(64, 3);
    auto fw_blob = MakeBlob(64, 9);

    zassert_true(ui_assets.Save("asset.bin", ui_blob));
    zassert_true(fw_assets.Save("asset.bin", fw_blob));

    auto loaded_ui = ui_assets.Load("asset.bin");
    auto loaded_fw = fw_assets.Load("asset.bin");

    zassert_mem_equal(loaded_ui.data(), ui_blob.data(), ui_blob.size());
    zassert_mem_equal(loaded_fw.data(), fw_blob.data(), fw_blob.size());

    zassert_true(ui_assets.Delete("asset.bin"));
    zassert_false(ui_assets.Exists("asset.bin"));
    zassert_true(fw_assets.Exists("asset.bin"));
}

ZTEST(asset_store, test_empty_assets_dir_targets_volume_root) {
    auto store = MakeStore("");
    auto blob = MakeBlob(16, 5);

    zassert_true(store.Save("asset.bin", blob));
    zassert_true(GetFsService()->Exists("asset.bin"));

    auto loaded = store.Load("asset.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_get_path_joins_the_directory_and_the_path) {
    // Nothing here reaches the volume.
    AssetStore nested(std::make_shared<UnavailableFsService>(), "a/b");
    AssetStore flat(std::make_shared<UnavailableFsService>(), "assets");
    AssetStore root(std::make_shared<UnavailableFsService>(), "");

    zassert_str_equal(nested.GetPath("x.bin").c_str(), "a/b/x.bin");
    zassert_str_equal(flat.GetPath("sub/x.bin").c_str(), "assets/sub/x.bin");
    zassert_str_equal(root.GetPath("x.bin").c_str(), "x.bin");

    zassert_str_equal(nested.GetDirectory().c_str(), "a/b");
    zassert_str_equal(root.GetDirectory().c_str(), "");
}

ZTEST(asset_store, test_get_free_space_falls_by_what_is_written) {
    auto store = MakeStore();
    constexpr size_t K_SIZE = 20000;

    const uint64_t free_before = store.GetFreeSpace();
    zassert_true(free_before > K_SIZE, "The test volume reports %d bytes free", static_cast<int>(free_before));

    zassert_true(store.Save("image.bin", MakeBlob(K_SIZE)));

    const uint64_t free_after = store.GetFreeSpace();

    // The volume hands out whole allocation units, so it loses a little more than was written.
    zassert_true(
        free_before >= free_after + K_SIZE, "%d bytes free before, %d after writing %d", static_cast<int>(free_before),
        static_cast<int>(free_after), static_cast<int>(K_SIZE)
    );

    zassert_true(store.Delete("image.bin"));
    zassert_true(store.GetFreeSpace() > free_after);
}

// Every operation must degrade to a failure instead of touching an unmounted volume.
ZTEST(asset_store, test_unavailable_fs_service_fails_gracefully) {
    AssetStore store(std::make_shared<UnavailableFsService>(), ASSETS_DIR);

    zassert_false(store.IsAvailable());
    zassert_false(store.Save("asset.bin", MakeBlob(16)));
    zassert_true(store.Load("asset.bin").empty());
    zassert_false(store.Delete("asset.bin"));
    zassert_false(store.Exists("asset.bin"));
    zassert_false(store.GetSize("asset.bin").has_value());
    zassert_equal(store.GetFreeSpace(), 0);
    zassert_false(store.RemoveStaged());

    std::vector<AssetEntry> entries;
    entries.push_back({ .name = "stale.bin", .size = 1, .index = 1 });
    size_t scanned = 7;
    size_t count = 9;

    zassert_equal(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count), -ENODEV);
    zassert_true(entries.empty());
    zassert_equal(scanned, 0);
    zassert_equal(count, 0);

    AssetReader reader;
    zassert_equal(store.OpenRead("asset.bin", reader), -ENODEV);
    zassert_false(reader.IsOpen());

    AssetWriter writer;
    zassert_equal(store.BeginWrite("asset.bin", writer), -ENODEV);
    zassert_false(writer.IsOpen());
}

// The service above fails every call by itself, which hides an operation that forgets to ask. Here the
// volume is reported unavailable while it is still mounted, as an SD card is that was pulled while its
// monitor is off: each operation has to stop at the question.
ZTEST(asset_store, test_every_operation_asks_whether_the_volume_is_available) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);

    zassert_true(store.Save("there.bin", MakeBlob(16)));

    scripted->unavailable = true;

    zassert_false(store.IsAvailable());
    zassert_false(store.Save("new.bin", MakeBlob(16)));
    zassert_false(GetFsService()->Exists("test_assets/new.bin"));
    zassert_true(store.Load("there.bin").empty());
    zassert_false(store.Delete("there.bin"));
    zassert_true(GetFsService()->Exists("test_assets/there.bin"));
    zassert_false(store.Exists("there.bin"));
    zassert_false(store.GetSize("there.bin").has_value());
    zassert_equal(store.GetFreeSpace(), 0);
    zassert_false(store.RemoveStaged());

    std::vector<AssetEntry> entries;
    size_t scanned = 0;
    size_t count = 0;

    zassert_equal(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count), -ENODEV);

    AssetReader reader;
    zassert_equal(store.OpenRead("there.bin", reader), -ENODEV);
    zassert_false(reader.IsOpen());

    AssetWriter writer;
    zassert_equal(store.BeginWrite("new.bin", writer), -ENODEV);
    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());

    // It was the question that stopped them: the volume works again once it is available.
    scripted->unavailable = false;

    zassert_true(store.Exists("there.bin"));
    zassert_equal(store.Load("there.bin").size(), 16);
}

// ======== Names ========

ZTEST(asset_store, test_is_valid_name_accepts_names_of_the_allowed_characters) {
    zassert_true(AssetStore::IsValidName("image.bin", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("a.bin", BIN_EXTENSIONS), "One character before the extension is enough");
    zassert_true(AssetStore::IsValidName("0.bin", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("IMG_01-final.bin", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("x.y.bin", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("a..b.bin", BIN_EXTENSIONS), "Dots inside a name are no path");
    zassert_true(AssetStore::IsValidName("-_-.bin", BIN_EXTENSIONS));
}

ZTEST(asset_store, test_is_valid_name_limits_the_length) {
    const std::string longest = std::string(MAX_NAME_LENGTH - 4, 'a') + ".bin";
    const std::string too_long = std::string(MAX_NAME_LENGTH - 3, 'a') + ".bin";

    zassert_equal(longest.size(), MAX_NAME_LENGTH);
    zassert_equal(too_long.size(), MAX_NAME_LENGTH + 1);

    zassert_true(AssetStore::IsValidName(longest, BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName(too_long, BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName(std::string(1000, 'a') + ".bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("", BIN_EXTENSIONS));
}

ZTEST(asset_store, test_is_valid_name_refuses_a_leading_dot) {
    zassert_false(AssetStore::IsValidName(".bin", BIN_EXTENSIONS), "The extension alone is no name");
    zassert_false(AssetStore::IsValidName(".hidden.bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("..bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName(".", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("..", BIN_EXTENSIONS));
}

ZTEST(asset_store, test_is_valid_name_refuses_separators_and_other_characters) {
    zassert_false(AssetStore::IsValidName("a/b.bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("../a.bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("a\\b.bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("a b.bin", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("a.bin ", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName(std::string("a\0b.bin", 7), BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName(std::string("a.bin\0", 6), BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("caf\xC3\xA9.bin", BIN_EXTENSIONS), "Names are ASCII only");

    for(const char c : std::string_view("\t\n:*?\"<>|;,=+[]%~!@#$^&'(){}")) {
        const std::string name = std::string("a") + c + "b.bin";
        zassert_false(
            AssetStore::IsValidName(name, BIN_EXTENSIONS), "Accepted a name with 0x%02x", static_cast<int>(c)
        );
    }
}

ZTEST(asset_store, test_is_valid_name_requires_one_of_the_extensions) {
    zassert_false(AssetStore::IsValidName("image", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("image.png", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("image.bin.txt", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("image.binx", BIN_EXTENSIONS));
    zassert_false(AssetStore::IsValidName("imagebin", BIN_EXTENSIONS), "The extension has its dot");
    zassert_false(AssetStore::IsValidName("bin", BIN_EXTENSIONS));

    constexpr std::array<std::string_view, 2> SEVERAL = { ".bin", ".json" };

    zassert_true(AssetStore::IsValidName("a.bin", SEVERAL));
    zassert_true(AssetStore::IsValidName("a.json", SEVERAL));
    zassert_false(AssetStore::IsValidName("a.lua", SEVERAL));

    zassert_false(
        AssetStore::IsValidName("a.bin", std::span<const std::string_view>{}), "No extension accepts nothing"
    );
}

ZTEST(asset_store, test_is_valid_name_compares_the_extension_without_case) {
    constexpr std::array<std::string_view, 2> SEVERAL = { ".bin", ".JSON" };

    zassert_true(AssetStore::IsValidName("image.BIN", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("IMAGE.Bin", BIN_EXTENSIONS));
    zassert_true(AssetStore::IsValidName("a.json", SEVERAL));
    zassert_true(AssetStore::IsValidName("a.Json", SEVERAL));
    zassert_false(AssetStore::IsValidName("a.jsonn", SEVERAL));
}

// ======== Listing ========

ZTEST(asset_store, test_list_keeps_the_files_with_valid_names) {
    auto store = MakeStore();

    zassert_true(WriteBlob("test_assets/alpha.bin", 10));
    zassert_true(WriteBlob("test_assets/beta.bin", 0));
    zassert_true(WriteBlob("test_assets/Gamma.BIN", 4097));

    const auto listing = ListPage(store, 0, 10);

    zassert_ok(listing.rc);
    zassert_equal(listing.count, 3);
    zassert_equal(listing.scanned, 3);
    zassert_equal(listing.entries.size(), 3);
    zassert_equal(EntryNamed(listing.entries, "alpha.bin").size, 10);
    zassert_equal(EntryNamed(listing.entries, "beta.bin").size, 0);
    zassert_equal(EntryNamed(listing.entries, "Gamma.BIN").size, 4097);
}

ZTEST(asset_store, test_list_skips_directories_and_invalid_names_but_counts_them) {
    auto store = MakeStore();

    zassert_true(WriteBlob("test_assets/ok.bin", 5));
    // A directory named like an asset, which is no file to serve.
    zassert_true(GetFsService()->CreateDirectory("test_assets/folder.bin"));
    zassert_true(WriteBlob("test_assets/notes.txt", 5));
    zassert_true(WriteBlob("test_assets/two words.bin", 5));
    zassert_true(WriteBlob("test_assets/.hidden.bin", 5));

    const auto listing = ListPage(store, 0, 10);

    zassert_ok(listing.rc);
    zassert_equal(listing.count, 5);
    zassert_equal(listing.scanned, 5, "Entries that are skipped still count as read");
    zassert_equal(listing.entries.size(), 1);
    zassert_str_equal(listing.entries[0].name.c_str(), "ok.bin");
    zassert_equal(listing.entries[0].size, 5);
    zassert_equal(listing.entries[0].index, PositionOf(ASSETS_DIR, "ok.bin"));
}

ZTEST(asset_store, test_list_pages_by_first_and_max_entries) {
    auto store = MakeStore();

    for(int i = 0; i < 5; ++i)
        zassert_true(WriteBlob("test_assets/file_" + std::to_string(i) + ".bin", i + 1));

    zassert_true(GetFsService()->CreateDirectory("test_assets/folder"));
    zassert_true(WriteBlob("test_assets/skipped.txt", 1));

    const auto all = ListPage(store, 0, 100);

    zassert_ok(all.rc);
    zassert_equal(all.count, 7);
    zassert_equal(all.scanned, 7);
    zassert_equal(all.entries.size(), 5);

    // Two entries at a time, each page listing on where the last one stopped.
    std::vector<AssetEntry> paged;
    size_t first = 0;
    size_t pages = 0;

    while(first < all.count) {
        const auto page = ListPage(store, first, 2);

        zassert_ok(page.rc);
        zassert_equal(page.count, all.count);
        zassert_true(
            page.scanned > 0 && page.scanned <= 2, "A page scanned %d entries", static_cast<int>(page.scanned)
        );

        for(const auto& entry : page.entries) {
            zassert_true(
                entry.index >= first && entry.index < first + page.scanned, "Entry %d is not in the page from %d",
                static_cast<int>(entry.index), static_cast<int>(first)
            );
            paged.push_back(entry);
        }

        first += page.scanned;
        pages++;
    }

    zassert_equal(pages, 4);
    zassert_equal(paged.size(), all.entries.size());

    for(size_t i = 0; i < paged.size(); ++i) {
        zassert_str_equal(paged[i].name.c_str(), all.entries[i].name.c_str());
        zassert_equal(paged[i].size, all.entries[i].size);
        zassert_equal(paged[i].index, all.entries[i].index);
    }
}

ZTEST(asset_store, test_list_past_the_end_and_with_no_room_lists_nothing) {
    auto store = MakeStore();

    zassert_true(WriteBlob("test_assets/a.bin", 1));
    zassert_true(WriteBlob("test_assets/b.bin", 1));

    const auto past_the_end = ListPage(store, 2, 10);

    zassert_ok(past_the_end.rc);
    zassert_true(past_the_end.entries.empty());
    zassert_equal(past_the_end.scanned, 0);
    zassert_equal(past_the_end.count, 2);

    const auto only_counted = ListPage(store, 0, 0);

    zassert_ok(only_counted.rc);
    zassert_true(only_counted.entries.empty());
    zassert_equal(only_counted.scanned, 0);
    zassert_equal(only_counted.count, 2);
}

ZTEST(asset_store, test_list_clears_what_the_caller_passed_in) {
    auto store = MakeStore();

    zassert_true(WriteBlob("test_assets/a.bin", 1));

    std::vector<AssetEntry> entries;
    entries.push_back({ .name = "stale.bin", .size = 1, .index = 5 });
    size_t scanned = 9;
    size_t count = 9;

    zassert_ok(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count));

    zassert_equal(entries.size(), 1);
    zassert_str_equal(entries[0].name.c_str(), "a.bin");
    zassert_equal(scanned, 1);
    zassert_equal(count, 1);
}

ZTEST(asset_store, test_list_of_a_missing_directory_lists_nothing) {
    auto store = MakeStore();

    // The constructor created the directory; it goes away again.
    zassert_true(GetFsService()->DeleteFile(ASSETS_DIR));

    std::vector<AssetEntry> entries;
    entries.push_back({ .name = "stale.bin", .size = 1, .index = 5 });
    size_t scanned = 7;
    size_t count = 9;

    zassert_ok(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count));
    zassert_true(entries.empty());
    zassert_equal(scanned, 0);
    zassert_equal(count, 0);
}

ZTEST(asset_store, test_list_of_a_file_in_place_of_the_directory_fails) {
    zassert_true(WriteBlob("blocked", 4));

    AssetStore store(GetFsService(), "blocked");

    std::vector<AssetEntry> entries;
    size_t scanned = 0;
    size_t count = 0;

    zassert_equal(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count), -ENOTDIR);
    zassert_true(entries.empty());
    zassert_equal(count, 0);
}

ZTEST(asset_store, test_list_of_the_volume_root_skips_the_staging_directory) {
    auto store = MakeStore("");
    AssetWriter writer;

    zassert_ok(store.BeginWrite("root.bin", writer));
    zassert_ok(writer.Append(MakeBlob(10)));
    zassert_ok(writer.Commit());

    const auto listing = ListPage(store, 0, 10);

    zassert_ok(listing.rc);
    zassert_equal(listing.count, 2, "The staging directory is in the root");
    zassert_equal(listing.scanned, 2);
    zassert_equal(listing.entries.size(), 1);
    zassert_str_equal(listing.entries[0].name.c_str(), "root.bin");
    zassert_equal(listing.entries[0].index, PositionOf("", "root.bin"));
}

// The volume is reported unavailable while it is still mounted, so ListDirectory() would list it.
ZTEST(asset_store, test_list_of_an_unavailable_volume_is_ENODEV) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);

    zassert_true(WriteBlob("test_assets/a.bin", 1));

    // Underneath, the directory lists.
    std::vector<DirectoryEntry> raw;
    size_t raw_count = 0;

    zassert_ok(GetFsService()->ListDirectory(ASSETS_DIR, 0, 10, raw, raw_count));
    zassert_equal(raw_count, 1);

    scripted->unavailable = true;

    std::vector<AssetEntry> entries;
    entries.push_back({ .name = "stale.bin", .size = 1, .index = 5 });
    size_t scanned = 7;
    size_t count = 9;

    zassert_equal(store.List(0, 10, BIN_EXTENSIONS, entries, scanned, count), -ENODEV);
    zassert_true(entries.empty());
    zassert_equal(scanned, 0);
    zassert_equal(count, 0);
}

// ======== Reading ========

ZTEST(asset_store, test_read_in_pieces_of_any_size_returns_the_content) {
    auto store = MakeStore();
    const auto blob = MakeBlob(10000, 11);

    zassert_true(store.Save("image.bin", blob));

    for(const size_t piece : READ_PIECES) {
        AssetReader reader;

        zassert_ok(store.OpenRead("image.bin", reader));
        zassert_true(reader.IsOpen());
        zassert_equal(reader.GetSize(), blob.size());

        const auto content = ReadAll(reader, piece);

        zassert_equal(content.size(), blob.size(), "Read in pieces of %d", static_cast<int>(piece));
        zassert_mem_equal(content.data(), blob.data(), blob.size());
    }
}

ZTEST(asset_store, test_read_back_files_around_the_buffer_size) {
    auto store = MakeStore();

    for(const size_t size : EDGE_SIZES) {
        const auto blob = MakeBlob(size, 21);

        zassert_true(store.Save("edge.bin", blob), "Saving %d bytes", static_cast<int>(size));

        AssetReader reader;

        zassert_ok(store.OpenRead("edge.bin", reader));
        zassert_equal(reader.GetSize(), size);

        const auto content = ReadAll(reader, 1000);

        AssertSameContent(content, blob);

        std::vector<uint8_t> beyond(8);
        zassert_equal(reader.Read(size, beyond), 0, "Reading at the end of %d bytes", static_cast<int>(size));
    }
}

ZTEST(asset_store, test_read_crossing_the_buffer_boundaries_returns_the_content) {
    auto store = MakeStore();
    const auto blob = MakeBlob(12000, 5);

    zassert_true(store.Save("image.bin", blob));

    AssetReader reader;
    zassert_ok(store.OpenRead("image.bin", reader));

    struct Request {
        size_t offset;
        size_t length;
    };

    // Across one boundary, exactly one block, across two, the whole file, and back to the start.
    constexpr std::array<Request, 8> REQUESTS = { {
        { 4090, 20 },
        { 4096, 4096 },
        { 4000, 5000 },
        { 8190, 10 },
        { 0, 12000 },
        { 11990, 10 },
        { 4095, 2 },
        { 100, 3 },
    } };

    for(const auto& request : REQUESTS) {
        std::vector<uint8_t> out(request.length);
        const ssize_t bytes_read = reader.Read(request.offset, out);

        zassert_equal(
            bytes_read, static_cast<ssize_t>(request.length), "Read of %d at %d returned %d",
            static_cast<int>(request.length), static_cast<int>(request.offset), static_cast<int>(bytes_read)
        );
        zassert_mem_equal(out.data(), blob.data() + request.offset, request.length);
    }
}

ZTEST(asset_store, test_read_at_and_past_the_size_returns_zero) {
    auto store = MakeStore();
    const auto blob = MakeBlob(5000, 3);

    zassert_true(store.Save("image.bin", blob));

    AssetReader reader;
    zassert_ok(store.OpenRead("image.bin", reader));

    std::vector<uint8_t> out(100);

    zassert_equal(reader.Read(5000, out), 0);
    zassert_equal(reader.Read(5001, out), 0);
    zassert_equal(reader.Read(1000000, out), 0);
    zassert_equal(reader.Read(0, std::span<uint8_t>{}), 0);

    // A request that runs past the end gets what is left.
    zassert_equal(reader.Read(4950, out), 50);
    zassert_mem_equal(out.data(), blob.data() + 4950, 50);
    zassert_equal(reader.Read(4999, out), 1);
    zassert_equal(out[0], blob[4999]);
}

ZTEST(asset_store, test_read_of_an_empty_file_returns_zero) {
    auto store = MakeStore();

    zassert_true(store.Save("empty.bin", std::span<const uint8_t>{}));

    AssetReader reader;
    zassert_ok(store.OpenRead("empty.bin", reader));
    zassert_true(reader.IsOpen());
    zassert_equal(reader.GetSize(), 0);

    std::vector<uint8_t> out(16);
    zassert_equal(reader.Read(0, out), 0);
}

ZTEST(asset_store, test_read_at_scattered_offsets_returns_the_content) {
    auto store = MakeStore();
    const auto blob = MakeBlob(10000, 17);

    zassert_true(store.Save("image.bin", blob));

    AssetReader reader;
    zassert_ok(store.OpenRead("image.bin", reader));

    // Forwards, backwards and into the block read last: each is served from the right place.
    constexpr std::array<size_t, 9> OFFSETS = { 9000, 10, 5000, 5100, 4095, 4096, 8191, 0, 9990 };

    for(const size_t offset : OFFSETS) {
        std::vector<uint8_t> out(10);

        zassert_equal(reader.Read(offset, out), 10);
        zassert_mem_equal(out.data(), blob.data() + offset, 10, "Read at %d", static_cast<int>(offset));
    }
}

ZTEST(asset_store, test_open_read_of_a_directory_is_ENOENT) {
    auto store = MakeStore();

    zassert_true(GetFsService()->CreateDirectory(std::string(ASSETS_DIR) + "/folder"));

    AssetReader reader;

    zassert_equal(store.OpenRead("folder", reader), -ENOENT, "FAT and littlefs say it differently");
    zassert_false(reader.IsOpen());
}

ZTEST(asset_store, test_open_read_of_a_missing_asset_is_ENOENT) {
    auto store = MakeStore();
    AssetReader reader;

    zassert_equal(store.OpenRead("missing.bin", reader), -ENOENT);
    zassert_false(reader.IsOpen());
    zassert_equal(reader.GetSize(), 0);

    zassert_equal(store.OpenRead("", reader), -ENOENT);
    zassert_false(reader.IsOpen());
}

ZTEST(asset_store, test_reader_that_was_never_opened_is_closed) {
    AssetReader reader;
    std::vector<uint8_t> out(8);

    zassert_false(reader.IsOpen());
    zassert_equal(reader.GetSize(), 0);
    zassert_equal(reader.Read(0, out), -EBADF);
}

ZTEST(asset_store, test_moved_reader_works_and_the_source_is_closed) {
    auto store = MakeStore();
    const auto blob = MakeBlob(9000, 8);

    zassert_true(store.Save("image.bin", blob));

    AssetReader first;
    zassert_ok(store.OpenRead("image.bin", first));

    AssetReader second(std::move(first));

    std::vector<uint8_t> out(16);

    // NOLINTNEXTLINE(bugprone-use-after-move): the case checks the state a moved-from object is left in
    zassert_false(first.IsOpen());
    zassert_equal(first.GetSize(), 0);
    zassert_equal(first.Read(0, out), -EBADF);
    zassert_true(second.IsOpen());
    zassert_equal(second.GetSize(), blob.size());

    // Part of the way through, to show that what is moved includes the position.
    zassert_equal(second.Read(4000, out), 16);
    zassert_mem_equal(out.data(), blob.data() + 4000, 16);

    AssetReader third;
    third = std::move(second);

    // NOLINTNEXTLINE(bugprone-use-after-move): the case checks the state a moved-from object is left in
    zassert_false(second.IsOpen());
    zassert_equal(second.Read(0, out), -EBADF);
    zassert_true(third.IsOpen());

    const auto content = ReadAll(third, 700);
    zassert_equal(content.size(), blob.size());
    zassert_mem_equal(content.data(), blob.data(), blob.size());

    third.Close();
    zassert_true(store.Delete("image.bin"));
}

ZTEST(asset_store, test_closing_a_reader_twice_is_harmless) {
    auto store = MakeStore();

    zassert_true(store.Save("image.bin", MakeBlob(100)));

    AssetReader reader;
    zassert_ok(store.OpenRead("image.bin", reader));

    reader.Close();
    reader.Close();

    std::vector<uint8_t> out(8);

    zassert_false(reader.IsOpen());
    zassert_equal(reader.GetSize(), 0);
    zassert_equal(reader.Read(0, out), -EBADF);

    // It can be opened again.
    zassert_ok(store.OpenRead("image.bin", reader));
    zassert_equal(reader.Read(0, out), 8);
}

// Both file systems hand out a few file slots, and one that is never given back stays lost.
ZTEST(asset_store, test_opening_and_closing_never_leak_file_slots) {
    auto store = MakeStore();

    zassert_true(store.Save("image.bin", MakeBlob(100)));
    zassert_true(GetFsService()->CreateDirectory(std::string(ASSETS_DIR) + "/folder"));

    // More rounds than either file system has slots.
    constexpr int ROUNDS = 12;

    for(int i = 0; i < ROUNDS; ++i) {
        AssetReader reader;
        AssetWriter writer;

        zassert_ok(store.OpenRead("image.bin", reader), "Round %d", i);
        reader.Close();

        zassert_equal(store.OpenRead("folder", reader), -ENOENT, "Round %d", i);
        zassert_equal(store.OpenRead("missing.bin", reader), -ENOENT, "Round %d", i);

        zassert_ok(store.BeginWrite("new.bin", writer), "Round %d", i);
        writer.Abort();

        zassert_ok(store.BeginWrite("new.bin", writer), "Round %d", i);
        zassert_ok(writer.Commit(), "Round %d", i);
    }

    // A reader or a writer that is opened again without being closed gives its slot back first.
    AssetReader reader;
    AssetWriter writer;

    for(int i = 0; i < ROUNDS; ++i) {
        zassert_ok(store.OpenRead("image.bin", reader), "Reopening round %d", i);
        zassert_ok(store.BeginWrite("again.bin", writer), "Reopening round %d", i);
    }

    // And so does one that a move replaces.
    for(int i = 0; i < ROUNDS; ++i) {
        AssetReader replacement;

        zassert_ok(store.OpenRead("image.bin", replacement), "Replacing round %d", i);
        reader = std::move(replacement);
    }

    writer.Abort();
}

ZTEST(asset_store, test_open_read_without_a_free_file_slot_is_ENOMEM) {
    auto store = MakeStore();

    zassert_true(store.Save("image.bin", MakeBlob(100)));

    constexpr size_t MAX_READERS = 16;
    std::array<AssetReader, MAX_READERS> readers;
    size_t opened = 0;
    int rc = 0;

    while(opened < MAX_READERS) {
        rc = store.OpenRead("image.bin", readers[opened]);
        if(rc != 0)
            break;

        opened++;
    }

    zassert_true(opened > 0, "Not even one file could be opened");
    zassert_equal(rc, -ENOMEM, "%d files were open and the next one gave %d", static_cast<int>(opened), rc);
    zassert_false(readers[opened].IsOpen());

    // A slot that is given back can be used again.
    readers[0].Close();
    zassert_ok(store.OpenRead("image.bin", readers[0]));
}

// ======== Staged writes ========

ZTEST(asset_store, test_staged_write_publishes_the_asset_on_commit) {
    auto store = MakeStore();
    const auto blob = MakeBlob(20000, 9);
    AssetWriter writer;

    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_true(writer.IsOpen());
    zassert_equal(writer.GetWritten(), 0);

    zassert_ok(AppendInPieces(writer, blob, ODD_PIECES));
    zassert_equal(writer.GetWritten(), blob.size());

    // Until the commit the asset does not exist: the data is in the staged file.
    zassert_true(StagedFileExists());
    zassert_false(store.Exists("image.bin"));

    zassert_ok(writer.Commit());

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());
    zassert_true(store.Exists("image.bin"));

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_staged_write_of_sizes_around_the_buffer_size) {
    auto store = MakeStore();

    for(const size_t size : EDGE_SIZES) {
        const auto blob = MakeBlob(size, 31);
        AssetWriter writer;

        zassert_ok(store.BeginWrite("edge.bin", writer), "Writing %d bytes", static_cast<int>(size));
        zassert_ok(AppendInPieces(writer, blob, ODD_PIECES));
        zassert_equal(writer.GetWritten(), size);
        zassert_ok(writer.Commit(), "Writing %d bytes", static_cast<int>(size));

        zassert_true(store.GetSize("edge.bin").has_value());
        zassert_equal(store.GetSize("edge.bin").value(), size);

        AssertSameContent(store.Load("edge.bin"), blob);
    }
}

ZTEST(asset_store, test_staged_write_in_one_append_returns_the_content) {
    auto store = MakeStore();
    const auto blob = MakeBlob(10000, 2);
    AssetWriter writer;

    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(blob));
    zassert_ok(writer.Append(std::span<const uint8_t>{}));
    zassert_equal(writer.GetWritten(), blob.size());
    zassert_ok(writer.Commit());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_commit_replaces_an_existing_asset) {
    auto store = MakeStore();
    const auto replacement = MakeBlob(300, 6);

    zassert_true(store.Save("image.bin", MakeBlob(5000, 1)));

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(replacement));
    zassert_ok(writer.Commit());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), replacement.size());
    zassert_mem_equal(loaded.data(), replacement.data(), replacement.size());
    zassert_false(StagedFileExists());
}

ZTEST(asset_store, test_commit_creates_a_missing_nested_directory) {
    AssetStore store(GetFsService(), "a/b");
    const auto blob = MakeBlob(500, 3);

    // The constructor created it; it goes away again.
    zassert_true(GetFsService()->Exists("a/b"));
    zassert_true(GetFsService()->DeleteFile("a/b"));
    zassert_true(GetFsService()->DeleteFile("a"));
    zassert_false(GetFsService()->Exists("a"));

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(blob));
    zassert_ok(writer.Commit());

    zassert_true(GetFsService()->Exists("a/b/image.bin"));

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_staged_write_to_the_volume_root) {
    auto store = MakeStore("");
    const auto blob = MakeBlob(100, 4);
    AssetWriter writer;

    zassert_ok(store.BeginWrite("root.bin", writer));
    zassert_ok(writer.Append(blob));
    zassert_ok(writer.Commit());

    zassert_true(GetFsService()->Exists("root.bin"));

    const auto loaded = store.Load("root.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_zero_length_commit_creates_an_empty_file) {
    auto store = MakeStore();
    AssetWriter writer;

    zassert_ok(store.BeginWrite("empty.bin", writer));
    zassert_ok(writer.Commit());

    zassert_false(writer.IsOpen());
    zassert_true(store.Exists("empty.bin"));
    zassert_true(store.GetSize("empty.bin").has_value());
    zassert_equal(store.GetSize("empty.bin").value(), 0);
    zassert_true(store.Load("empty.bin").empty());
    zassert_false(StagedFileExists());
}

ZTEST(asset_store, test_abort_deletes_the_staged_file_and_keeps_the_asset) {
    auto store = MakeStore();
    const auto original = MakeBlob(400, 2);

    zassert_true(store.Save("image.bin", original));

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(AppendInPieces(writer, MakeBlob(9000, 5), ODD_PIECES));
    zassert_true(StagedFileExists());

    writer.Abort();

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), original.size());
    zassert_mem_equal(loaded.data(), original.data(), original.size());

    // Nothing is left to abort, and nothing to commit.
    writer.Abort();
    zassert_equal(writer.Commit(), -EBADF);
}

ZTEST(asset_store, test_destroying_a_writer_aborts_the_write) {
    auto store = MakeStore();
    const auto original = MakeBlob(400, 2);

    zassert_true(store.Save("image.bin", original));

    {
        AssetWriter writer;

        zassert_ok(store.BeginWrite("image.bin", writer));
        zassert_ok(writer.Append(MakeBlob(5000, 5)));
        zassert_ok(store.BeginWrite("other.bin", writer), "A second write replaces the first");
        zassert_ok(writer.Append(MakeBlob(10, 5)));
        zassert_true(StagedFileExists());
    }

    zassert_false(StagedFileExists());
    zassert_false(store.Exists("other.bin"));

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), original.size());
    zassert_mem_equal(loaded.data(), original.data(), original.size());
}

ZTEST(asset_store, test_begin_write_truncates_a_stale_staged_file) {
    auto store = MakeStore();
    const auto blob = MakeBlob(100, 12);

    LeaveStagedFile(5000);

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(blob));
    zassert_ok(writer.Commit());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size(), "The stale file's tail is still there");
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_remove_staged_deletes_a_file_left_behind) {
    auto store = MakeStore();

    zassert_true(store.RemoveStaged(), "Nothing is left, which is as good");

    LeaveStagedFile(300);
    zassert_true(StagedFileExists());

    zassert_true(store.RemoveStaged());
    zassert_false(StagedFileExists());
    zassert_true(store.RemoveStaged());
}

ZTEST(asset_store, test_closed_writer_refuses_writes) {
    AssetWriter writer;

    zassert_false(writer.IsOpen());
    zassert_equal(writer.GetWritten(), 0);
    zassert_equal(writer.Append(MakeBlob(10)), -EBADF);
    zassert_equal(writer.Commit(), -EBADF);

    writer.Abort();
    zassert_false(writer.IsOpen());
}

ZTEST(asset_store, test_begin_write_restarts_an_open_writer) {
    auto store = MakeStore();
    AssetWriter writer;

    // Each round abandons the writer's file and its slot, unless BeginWrite() gives them back.
    for(int i = 0; i < 12; ++i) {
        zassert_ok(store.BeginWrite("image.bin", writer), "Round %d", i);
        zassert_equal(writer.GetWritten(), 0);
        zassert_ok(writer.Append(MakeBlob(100, static_cast<uint8_t>(i))));
    }

    zassert_ok(writer.Commit());

    const auto expected = MakeBlob(100, 11);
    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), expected.size());
    zassert_mem_equal(loaded.data(), expected.data(), expected.size());
}

ZTEST(asset_store, test_moved_writer_continues_the_staged_write) {
    auto store = MakeStore();
    const auto blob = MakeBlob(6000, 12);

    AssetWriter first;
    zassert_ok(store.BeginWrite("image.bin", first));
    zassert_ok(first.Append(std::span<const uint8_t>(blob.data(), 5000)));

    AssetWriter second(std::move(first));

    // NOLINTNEXTLINE(bugprone-use-after-move): the case checks the state a moved-from object is left in
    zassert_false(first.IsOpen());
    zassert_equal(first.GetWritten(), 0);
    zassert_equal(first.Append(blob), -EBADF);
    zassert_equal(first.Commit(), -EBADF);
    zassert_true(second.IsOpen());
    zassert_equal(second.GetWritten(), 5000);

    zassert_ok(second.Append(std::span<const uint8_t>(blob.data() + 5000, 1000)));

    AssetWriter third;
    third = std::move(second);

    // NOLINTNEXTLINE(bugprone-use-after-move): the case checks the state a moved-from object is left in
    zassert_false(second.IsOpen());
    zassert_true(third.IsOpen());
    zassert_equal(third.GetWritten(), blob.size());
    zassert_ok(third.Commit());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(asset_store, test_move_assignment_aborts_the_write_it_replaces) {
    auto store = MakeStore();
    AssetWriter writer;

    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(MakeBlob(100)));
    zassert_true(StagedFileExists());

    writer = AssetWriter();

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());
    zassert_false(store.Exists("image.bin"));
}

ZTEST(asset_store, test_commit_that_cannot_rename_deletes_the_staged_file) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);
    const auto original = MakeBlob(400, 2);

    zassert_true(store.Save("image.bin", original));

    scripted->rename_result = -EIO;

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(MakeBlob(9000, 5)));

    zassert_equal(writer.Commit(), -EIO);

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());

    const auto loaded = store.Load("image.bin");

    zassert_equal(loaded.size(), original.size());
    zassert_mem_equal(loaded.data(), original.data(), original.size());
}

ZTEST(asset_store, test_commit_that_cannot_create_the_directory_deletes_the_staged_file) {
    const auto scripted = std::make_shared<ScriptedFsService>(GetFsService());
    AssetStore store(scripted, ASSETS_DIR);

    zassert_true(GetFsService()->DeleteFile(ASSETS_DIR));
    scripted->failing_directory = ASSETS_DIR;

    AssetWriter writer;
    zassert_ok(store.BeginWrite("image.bin", writer));
    zassert_ok(writer.Append(MakeBlob(100)));

    zassert_equal(writer.Commit(), -EIO);

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());
    zassert_false(GetFsService()->Exists(ASSETS_DIR));
}

// FAT reports a full volume as a short write with no error, littlefs as -ENOSPC; both come out as
// -ENOSPC. The writer then stays failed, so that a file with a hole in it is never published.
ZTEST(asset_store, test_append_to_a_full_volume_is_ENOSPC_and_commit_refuses) {
    auto store = MakeStore();
    const auto kept = MakeBlob(100, 3);

    zassert_true(store.Save("keep.bin", kept));

    AssetWriter writer;
    zassert_ok(store.BeginWrite("big.bin", writer));

    size_t appended = 0;

    zassert_equal(AppendUntilFull(writer, appended), -ENOSPC);
    zassert_true(appended > 0, "The volume was full from the start");
    zassert_true(writer.IsOpen(), "A failed write leaves the writer to its caller");

    zassert_equal(writer.Append(MakeBlob(10)), -ENOSPC);
    zassert_equal(writer.Commit(), -ENOSPC);

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());
    zassert_false(store.Exists("big.bin"));

    // What it took is given back: the volume takes a new asset, and the old one is untouched.
    const auto small = MakeBlob(5000, 9);
    AssetWriter next;

    zassert_ok(store.BeginWrite("small.bin", next));
    zassert_ok(next.Append(small));
    zassert_ok(next.Commit());

    const auto loaded_small = store.Load("small.bin");
    const auto loaded_kept = store.Load("keep.bin");

    zassert_equal(loaded_small.size(), small.size());
    zassert_mem_equal(loaded_small.data(), small.data(), small.size());
    zassert_equal(loaded_kept.size(), kept.size());
    zassert_mem_equal(loaded_kept.data(), kept.data(), kept.size());
}

ZTEST(asset_store, test_abort_gives_back_the_space_of_a_full_volume) {
    auto store = MakeStore();
    AssetWriter writer;

    zassert_ok(store.BeginWrite("big.bin", writer));

    size_t appended = 0;

    zassert_equal(AppendUntilFull(writer, appended), -ENOSPC);
    zassert_true(appended > 0, "The volume was full from the start");

    writer.Abort();

    zassert_false(writer.IsOpen());
    zassert_false(StagedFileExists());

    // Half of what the failed write took is free again, and a new write of it goes through.
    const auto block = MakeBlob(AssetWriter::BUFFER_SIZE, 4);
    AssetWriter next;

    zassert_ok(store.BeginWrite("again.bin", next));

    for(size_t written = 0; written < appended / 2; written += block.size())
        zassert_ok(next.Append(block));

    zassert_ok(next.Commit());

    zassert_true(store.GetSize("again.bin").has_value());
    zassert_true(store.GetSize("again.bin").value() >= appended / 2);
}

// ======== Blocks ========

// Whatever the sizes of the caller's requests, the file system sees reads of one block each, at the
// start of the block, and no seek from one block to the next.
ZTEST(asset_store, test_reader_reads_the_file_in_aligned_blocks) {
    auto store = MakeStore();
    auto& recorder = Recorder();

    for(const size_t size : BLOCK_SIZES) {
        const auto blob = MakeBlob(size, 41);

        zassert_true(store.Save("blocks.bin", blob), "Saving %d bytes", static_cast<int>(size));

        AssetReader reader;
        zassert_ok(store.OpenRead("blocks.bin", reader));

        std::vector<uint8_t> content;

        recorder.Start();

        // Requests of odd sizes, one after the other.
        for(size_t i = 0; content.size() < size; ++i) {
            std::vector<uint8_t> out(ODD_PIECES[i % ODD_PIECES.size()]);
            const ssize_t bytes_read = reader.Read(content.size(), out);

            if(bytes_read <= 0)
                break;

            content.insert(content.end(), out.begin(), out.begin() + bytes_read);
        }

        recorder.Stop();

        AssertSameContent(content, blob);
        AssertBlockCalls(recorder.reads, size, AssetReader::BUFFER_SIZE);
        zassert_equal(
            recorder.seeks, 0, "A block that follows the last one needs no seek (%d bytes)", static_cast<int>(size)
        );
        zassert_true(recorder.writes.empty());
    }
}

// A request outside the block the reader holds seeks to the start of the block it needs, and no
// further: the block that follows the one read last is reached without a seek.
ZTEST(asset_store, test_reader_seeks_only_to_the_start_of_a_block) {
    auto store = MakeStore();
    auto& recorder = Recorder();
    const auto blob = MakeBlob(10000, 45);

    zassert_true(store.Save("blocks.bin", blob));

    AssetReader reader;
    zassert_ok(store.OpenRead("blocks.bin", reader));

    std::vector<uint8_t> second_block(10);
    std::vector<uint8_t> first_block(10);
    std::vector<uint8_t> across(20);

    recorder.Start();

    // The second block, with the file position at the start of the file: a seek.
    const ssize_t in_second = reader.Read(5000, second_block);
    // Back to the first: a seek.
    const ssize_t in_first = reader.Read(10, first_block);
    // The end of the first, held already, and the start of the second, which follows it: no seek.
    const ssize_t across_both = reader.Read(4090, across);

    recorder.Stop();

    zassert_equal(in_second, 10);
    zassert_mem_equal(second_block.data(), blob.data() + 5000, 10);
    zassert_equal(in_first, 10);
    zassert_mem_equal(first_block.data(), blob.data() + 10, 10);
    zassert_equal(across_both, 20);
    zassert_mem_equal(across.data(), blob.data() + 4090, 20);

    zassert_equal(recorder.reads.size(), 3);
    zassert_equal(recorder.reads[0].position, 4096);
    zassert_equal(recorder.reads[0].size, 4096);
    zassert_equal(recorder.reads[1].position, 0);
    zassert_equal(recorder.reads[1].size, 4096);
    zassert_equal(recorder.reads[2].position, 4096);
    zassert_equal(recorder.reads[2].size, 4096);
    zassert_equal(recorder.seeks, 2);
}

// The staged file is written a block at a time: whole blocks as the writer's buffer fills, and what
// is left of the last one when the write is committed. It is only ever appended to.
ZTEST(asset_store, test_writer_writes_the_file_in_aligned_blocks) {
    auto store = MakeStore();
    auto& recorder = Recorder();

    for(const size_t size : BLOCK_SIZES) {
        const auto blob = MakeBlob(size, 43);
        AssetWriter writer;

        zassert_ok(store.BeginWrite("blocks.bin", writer), "Writing %d bytes", static_cast<int>(size));

        recorder.Start();

        // Appends of odd sizes, one after the other.
        const int append_rc = AppendInPieces(writer, blob, ODD_PIECES);
        const size_t written_by_appends = recorder.writes.size();
        const int commit_rc = writer.Commit();

        recorder.Stop();

        zassert_ok(append_rc, "Appending %d bytes", static_cast<int>(size));
        zassert_ok(commit_rc, "Committing %d bytes", static_cast<int>(size));

        // The whole blocks went out as the buffer filled; only the rest waited for the commit.
        zassert_equal(
            written_by_appends, size / AssetWriter::BUFFER_SIZE, "Writes before the commit for %d bytes",
            static_cast<int>(size)
        );
        AssertBlockCalls(recorder.writes, size, AssetWriter::BUFFER_SIZE);
        zassert_equal(recorder.seeks, 0, "The staged file is only appended to (%d bytes)", static_cast<int>(size));
        zassert_true(recorder.reads.empty());

        AssertSameContent(store.Load("blocks.bin"), blob);
    }
}
