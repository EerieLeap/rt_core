#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/fs/services/i_fs_service.h"

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;

namespace {

constexpr std::string_view kAssetsDir = "test_assets";

std::shared_ptr<FsService> GetFsService() {
    static std::shared_ptr<FsService> fs_service;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        fs_service->Initialize();
    }

    return fs_service;
}

void ResetFs(void* = nullptr) {
    GetFsService()->DeleteRecursive();
}

AssetsManager MakeAssetsManager(std::string_view assets_dir = kAssetsDir) {
    return AssetsManager(GetFsService(), assets_dir);
}

std::vector<uint8_t> MakeBlob(size_t size, uint8_t seed = 0) {
    std::vector<uint8_t> blob(size);

    for(size_t i = 0; i < size; ++i)
        blob[i] = static_cast<uint8_t>((i + seed) & 0xFF);

    return blob;
}

// Every operation must degrade to a failure instead of touching an unmounted volume.
class UnavailableFsService : public IFsService {
public:
    bool Initialize() override { return false; }
    bool IsAvailable() const override { return false; }
    bool WriteFile(std::string_view, const void*, size_t, bool) override { return false; }
    bool ReadFile(std::string_view, void*, size_t, size_t& out_len) override { out_len = 0; return false; }
    int OpenFile(std::string_view, fs_mode_t, fs_file_t*) override { return -ENODEV; }
    bool CreateDirectory(std::string_view) override { return false; }
    bool Exists(std::string_view) const override { return false; }
    bool DeleteFile(std::string_view) override { return false; }
    bool DeleteRecursive(std::string_view) override { return false; }
    std::vector<std::string> ListFiles(std::string_view) const override { return {}; }
    std::optional<size_t> GetFileSize(std::string_view) const override { return std::nullopt; }
    uint64_t GetTotalSpace() const override { return 0; }
    uint64_t GetUsedSpace() const override { return 0; }
    bool Format() override { return false; }
};

} // namespace

ZTEST_SUITE(assets_manager, NULL, NULL, ResetFs, ResetFs, NULL);

ZTEST(assets_manager, test_construction_creates_assets_dir) {
    zassert_false(GetFsService()->Exists(kAssetsDir));

    auto assets_manager = MakeAssetsManager();

    zassert_true(GetFsService()->Exists(kAssetsDir));
    zassert_true(assets_manager.GetAssetsDir() == kAssetsDir);
}

ZTEST(assets_manager, test_construction_keeps_existing_assets_dir) {
    auto blob = MakeBlob(16);

    {
        auto assets_manager = MakeAssetsManager();
        zassert_true(assets_manager.Save("asset.bin", blob));
    }

    auto assets_manager = MakeAssetsManager();

    zassert_true(assets_manager.Exists("asset.bin"));
}

ZTEST(assets_manager, test_save_and_load_roundtrip) {
    auto assets_manager = MakeAssetsManager();
    auto blob = MakeBlob(512, 7);

    zassert_true(assets_manager.Save("image.bin", blob));

    auto loaded = assets_manager.Load("image.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(assets_manager, test_save_writes_below_assets_dir) {
    auto assets_manager = MakeAssetsManager();
    auto blob = MakeBlob(8);

    zassert_true(assets_manager.Save("image.bin", blob));

    zassert_true(GetFsService()->Exists(std::string(kAssetsDir) + "/image.bin"));
    zassert_false(GetFsService()->Exists("image.bin"));
}

ZTEST(assets_manager, test_save_overwrites_previous_content) {
    auto assets_manager = MakeAssetsManager();

    zassert_true(assets_manager.Save("image.bin", MakeBlob(256, 1)));

    auto smaller_blob = MakeBlob(32, 2);
    zassert_true(assets_manager.Save("image.bin", smaller_blob));

    auto loaded = assets_manager.Load("image.bin");

    zassert_equal(loaded.size(), smaller_blob.size());
    zassert_mem_equal(loaded.data(), smaller_blob.data(), smaller_blob.size());
}

ZTEST(assets_manager, test_save_empty_data_produces_empty_load) {
    auto assets_manager = MakeAssetsManager();

    zassert_true(assets_manager.Save("empty.bin", std::span<const uint8_t>{}));
    zassert_true(assets_manager.Exists("empty.bin"));
    zassert_true(assets_manager.Load("empty.bin").empty());
}

ZTEST(assets_manager, test_load_missing_asset_returns_empty) {
    auto assets_manager = MakeAssetsManager();

    zassert_true(assets_manager.Load("missing.bin").empty());
}

ZTEST(assets_manager, test_exists_reflects_save_and_delete) {
    auto assets_manager = MakeAssetsManager();

    zassert_false(assets_manager.Exists("image.bin"));

    zassert_true(assets_manager.Save("image.bin", MakeBlob(16)));
    zassert_true(assets_manager.Exists("image.bin"));

    zassert_true(assets_manager.Delete("image.bin"));
    zassert_false(assets_manager.Exists("image.bin"));
    zassert_true(assets_manager.Load("image.bin").empty());
}

ZTEST(assets_manager, test_delete_missing_asset_fails) {
    auto assets_manager = MakeAssetsManager();

    zassert_false(assets_manager.Delete("missing.bin"));
}

ZTEST(assets_manager, test_managers_with_different_dirs_are_isolated) {
    auto ui_assets = MakeAssetsManager("ui_assets");
    auto fw_assets = MakeAssetsManager("fw_assets");

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

ZTEST(assets_manager, test_empty_assets_dir_targets_volume_root) {
    auto assets_manager = MakeAssetsManager("");
    auto blob = MakeBlob(16, 5);

    zassert_true(assets_manager.Save("asset.bin", blob));
    zassert_true(GetFsService()->Exists("asset.bin"));

    auto loaded = assets_manager.Load("asset.bin");

    zassert_equal(loaded.size(), blob.size());
    zassert_mem_equal(loaded.data(), blob.data(), blob.size());
}

ZTEST(assets_manager, test_unavailable_fs_service_fails_gracefully) {
    AssetsManager assets_manager(std::make_shared<UnavailableFsService>(), kAssetsDir);

    zassert_false(assets_manager.Save("asset.bin", MakeBlob(16)));
    zassert_true(assets_manager.Load("asset.bin").empty());
    zassert_false(assets_manager.Delete("asset.bin"));
    zassert_false(assets_manager.Exists("asset.bin"));
}
