#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_store.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/fs/services/i_fs_service.h"

// Shared by the store tests and the registry tests. The store tests also build into the assets_fat
// suite, where the board's internal volume is FAT instead of littlefs.
namespace assets_test {

using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;

/** The internal volume the board's devicetree mounts. */
inline std::shared_ptr<FsService> GetFsService() {
    static std::shared_ptr<FsService> fs_service;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Initialize(), "The internal volume is not mounted");
    }

    return fs_service;
}

inline std::vector<uint8_t> MakeBlob(size_t size, uint8_t seed = 0) {
    std::vector<uint8_t> blob(size);

    for(size_t i = 0; i < size; ++i)
        blob[i] = static_cast<uint8_t>((i + seed) & 0xFF);

    return blob;
}

// Every operation must degrade to a failure instead of touching an unmounted volume.
class UnavailableFsService : public IFsService {
public:
    bool Initialize() override {
        return false;
    }
    bool IsAvailable() const override {
        return false;
    }
    bool WriteFile(std::string_view, const void*, size_t, bool) override {
        return false;
    }
    bool ReadFile(std::string_view, void*, size_t, size_t& out_len) override {
        out_len = 0;
        return false;
    }
    int OpenFile(std::string_view, fs_mode_t, fs_file_t*) override {
        return -ENODEV;
    }
    bool CreateDirectory(std::string_view) override {
        return false;
    }
    bool Exists(std::string_view) const override {
        return false;
    }
    bool DeleteFile(std::string_view) override {
        return false;
    }
    bool DeleteRecursive(std::string_view) override {
        return false;
    }
    std::vector<std::string> ListFiles(std::string_view) const override {
        return {};
    }
    int ListDirectory(
        std::string_view,
        size_t,
        size_t,
        std::vector<DirectoryEntry>& entries,
        size_t& count
    ) const override {
        entries.clear();
        count = 0;
        return -ENODEV;
    }
    std::optional<size_t> GetFileSize(std::string_view) const override {
        return std::nullopt;
    }
    uint64_t GetTotalSpace() const override {
        return 0;
    }
    uint64_t GetUsedSpace() const override {
        return 0;
    }
    bool Format() override {
        return false;
    }
    int Rename(std::string_view, std::string_view) override {
        return -ENODEV;
    }
};

/** A store on a volume that is not there, for tests that never reach the file system. */
inline std::shared_ptr<AssetStore> MakeUnavailableStore(std::string_view directory = "dir") {
    return std::make_shared<AssetStore>(std::make_shared<UnavailableFsService>(), directory);
}

} // namespace assets_test
