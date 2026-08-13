#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "utilities/string/filesystem_path.hpp"

#include "i_fs_service.h"

namespace eerie_leap::subsys::fs::services {

using eerie_leap::utilities::string::FilesystemPath;

class FsService : public IFsService {
private:
    static constexpr size_t PATH_BUFFER_SIZE = 256;

    bool DeleteDirectoryContents(FilesystemPath<PATH_BUFFER_SIZE>* full_path);

protected:
    // Not owned: fs_mount() links this very entry into the global mount list, so it has to be
    // the devicetree fstab entry and not a copy of it.
    fs_mount_t* mountpoint_;
    mutable k_mutex mutex_;

    bool TryBuildPath(std::string_view relative_path, FilesystemPath<PATH_BUFFER_SIZE>& full_path) const;

    bool Mount();
    void Unmount();
    bool IsMounted() const;

public:
    explicit FsService(fs_mount_t* mountpoint);

    FsService(const FsService&) = delete;
    FsService& operator=(const FsService&) = delete;
    FsService(FsService&&) = delete;
    FsService& operator=(FsService&&) = delete;

    using IFsService::DeleteRecursive;
    using IFsService::ListFiles;
    using IFsService::WriteFile;

    bool Initialize() override;
    bool IsAvailable() const override;
    bool WriteFile(std::string_view relative_path, const void* data_p, size_t data_size, bool append) override;
    bool ReadFile(std::string_view relative_path, void* data_p, size_t data_size, size_t& out_len) override;
    int OpenFile(std::string_view relative_path, fs_mode_t flags, fs_file_t* file_p) override;
    bool CreateDirectory(std::string_view relative_path) override;
    bool Exists(std::string_view relative_path) const override;
    bool DeleteFile(std::string_view relative_path) override;
    bool DeleteRecursive(std::string_view relative_path) override;
    std::vector<std::string> ListFiles(std::string_view relative_path) const override;
    std::optional<size_t> GetFileSize(std::string_view relative_path) const override;
    uint64_t GetTotalSpace() const override;
    uint64_t GetUsedSpace() const override;
    bool Format() override;
};

} // namespace eerie_leap::subsys::fs::services
