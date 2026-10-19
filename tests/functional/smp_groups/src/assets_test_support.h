#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/fs/services/i_fs_service.h"

// The volume the assets group tests serve files from, and what they need around it.
namespace assets_group_test {

using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;

/** The internal volume (littlefs on the simulated flash) the board's devicetree mounts. */
inline std::shared_ptr<FsService> GetSimFsService() {
    static std::shared_ptr<FsService> fs_service;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        zassert_true(fs_service->Initialize(), "The internal volume is not mounted");
    }

    return fs_service;
}

/**
 * A volume that can go away and come back, as an SD card does: while @p available is false it is
 * not mounted for everyone who asks, and fails what they ask for. Otherwise it is the volume it wraps.
 *
 * @p available is set by the test thread between requests and read on the configuration work queue.
 */
class SwitchableFsService : public IFsService {
public:
    std::atomic<bool> available;
    // Added to the wrapped volume's mount generation: a test counts a remount of the card here.
    std::atomic<uint32_t> remounts{ 0 };

    using IFsService::DeleteRecursive;
    using IFsService::ListFiles;
    using IFsService::WriteFile;

    explicit SwitchableFsService(std::shared_ptr<IFsService> inner, bool is_available = true) :
        available(is_available),
        inner_(std::move(inner)) {}

    bool Initialize() override {
        return inner_->Initialize();
    }
    bool IsAvailable() const override {
        return available && inner_->IsAvailable();
    }

    bool WriteFile(std::string_view path, const void* data, size_t size, bool append) override {
        return available && inner_->WriteFile(path, data, size, append);
    }

    bool ReadFile(std::string_view path, void* data, size_t size, size_t& out_len) override {
        if(!available) {
            out_len = 0;
            return false;
        }

        return inner_->ReadFile(path, data, size, out_len);
    }

    int OpenFile(std::string_view path, fs_mode_t flags, fs_file_t* file) override {
        return available ? inner_->OpenFile(path, flags, file) : -ENODEV;
    }

    bool CreateDirectory(std::string_view path) override {
        return available && inner_->CreateDirectory(path);
    }
    bool Exists(std::string_view path) const override {
        return available && inner_->Exists(path);
    }
    bool DeleteFile(std::string_view path) override {
        return available && inner_->DeleteFile(path);
    }
    bool DeleteRecursive(std::string_view path) override {
        return available && inner_->DeleteRecursive(path);
    }
    int Rename(std::string_view from, std::string_view to) override {
        return available ? inner_->Rename(from, to) : -ENODEV;
    }

    std::vector<std::string> ListFiles(std::string_view path) const override {
        return available ? inner_->ListFiles(path) : std::vector<std::string>{};
    }

    int ListDirectory(
        std::string_view path,
        size_t first,
        size_t max_entries,
        std::vector<DirectoryEntry>& entries,
        size_t& count
    ) const override {

        if(!available) {
            entries.clear();
            count = 0;

            return -ENODEV;
        }

        return inner_->ListDirectory(path, first, max_entries, entries, count);
    }

    std::optional<size_t> GetFileSize(std::string_view path) const override {
        return available ? inner_->GetFileSize(path) : std::nullopt;
    }

    uint64_t GetTotalSpace() const override {
        return available ? inner_->GetTotalSpace() : 0;
    }
    uint64_t GetUsedSpace() const override {
        return available ? inner_->GetUsedSpace() : 0;
    }
    bool Format() override {
        return available && inner_->Format();
    }
    uint32_t GetMountGeneration() const override {
        return inner_->GetMountGeneration() + remounts;
    }

private:
    std::shared_ptr<IFsService> inner_;
};

} // namespace assets_group_test
