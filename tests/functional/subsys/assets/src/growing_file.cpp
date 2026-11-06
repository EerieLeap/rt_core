// The littlefs `assets` suite compiles this file empty; assets_fat runs it.
#ifdef CONFIG_FAT_FILESYSTEM_ELM

#include <cstdint>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_store.h"

#include "assets_test_support.h"

// What the `assets` group relies on to read the log being written: a second handle of a file that
// another handle has open for writing on FatFs. Zephyr builds FatFs without sharing control
// (FF_FS_LOCK 0), so the second handle opens, and its size is the one in the directory entry, which
// only the writer's fs_sync() updates. The bytes below that size are on the disk whatever the
// writer's buffers hold. A Zephyr update that turns sharing control on, or changes when the size is
// published, fails here and not on the bench.
//
// FAT only: littlefs does not behave so, and the assets suite does not run this.

using eerie_leap::subsys::assets::AssetReader;
using eerie_leap::subsys::assets::AssetStore;

using assets_test::GetFsService;
using assets_test::MakeBlob;

namespace {

constexpr const char* DIRECTORY = "growing";
constexpr const char* NAME = "run.mdf";

void ResetFs(void* = nullptr) {
    GetFsService()->DeleteRecursive();
}

std::vector<uint8_t> ReadAll(AssetReader& reader) {
    std::vector<uint8_t> content(reader.GetSize());

    if(!content.empty())
        zassert_equal(reader.Read(0, content), static_cast<ssize_t>(content.size()));

    return content;
}

// Appends @p blob through @p file and syncs, as the log writer does.
void AppendAndSync(fs_file_t& file, const std::vector<uint8_t>& blob) {
    zassert_equal(fs_write(&file, blob.data(), blob.size()), static_cast<ssize_t>(blob.size()));
    zassert_ok(fs_sync(&file));
}

} // namespace

ZTEST_SUITE(asset_growing_file, NULL, NULL, ResetFs, ResetFs, NULL);

ZTEST(asset_growing_file, test_a_reader_sees_the_size_at_the_last_sync_and_the_growth_after_the_next) {
    AssetStore store(GetFsService(), DIRECTORY);
    const auto whole = MakeBlob(9000, 5);
    const std::vector<uint8_t> first(whole.begin(), whole.begin() + 3000);
    const std::vector<uint8_t> second(whole.begin() + 3000, whole.begin() + 7000);
    const std::vector<uint8_t> third(whole.begin() + 7000, whole.end());

    fs_file_t writer;
    fs_file_t_init(&writer);
    zassert_ok(GetFsService()->OpenFile(store.GetPath(NAME), FS_O_CREATE | FS_O_WRITE, &writer));

    // The header: written and synced.
    AppendAndSync(writer, first);

    AssetReader reader;
    zassert_ok(store.OpenRead(NAME, reader), "A second handle opens a file that is open for writing");
    zassert_equal(reader.GetSize(), first.size(), "The size at the last sync");
    zassert_true(ReadAll(reader) == first);

    // Written but not synced: a reader that opens now still sees the size at the sync.
    zassert_equal(fs_write(&writer, second.data(), second.size()), static_cast<ssize_t>(second.size()));

    AssetReader unsynced;
    zassert_ok(store.OpenRead(NAME, unsynced));
    zassert_equal(unsynced.GetSize(), first.size(), "Unsynced bytes are not published");
    zassert_true(ReadAll(unsynced) == first);
    unsynced.Close();

    // The reader that was open keeps the size it opened with.
    zassert_equal(reader.GetSize(), first.size());

    zassert_ok(fs_sync(&writer));

    AssetReader grown;
    zassert_ok(store.OpenRead(NAME, grown));
    zassert_equal(grown.GetSize(), first.size() + second.size(), "The growth shows after the next sync");
    zassert_true(ReadAll(grown) == std::vector<uint8_t>(whole.begin(), whole.begin() + 7000));
    grown.Close();

    // And once more.
    AppendAndSync(writer, third);

    AssetReader last;
    zassert_ok(store.OpenRead(NAME, last));
    zassert_equal(last.GetSize(), whole.size());
    zassert_true(ReadAll(last) == whole);
    last.Close();

    // The first reader still reads the bytes it opened with, the same as the others read.
    std::vector<uint8_t> again(first.size());
    zassert_equal(reader.Read(0, again), static_cast<ssize_t>(again.size()));
    zassert_true(again == first);

    reader.Close();
    zassert_ok(fs_close(&writer));
}

#endif // CONFIG_FAT_FILESYSTEM_ELM
