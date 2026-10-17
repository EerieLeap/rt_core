#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"

namespace eerie_leap::tests {

using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::IFsService;

// An SD card holding the files a script_path names, which the validators check; mounted or not, as
// an SdmmcService is. Like FsService, nothing exists while it is not mounted. The rest does nothing.
class ScriptCard final : public IFsService {
public:
    std::set<std::string, std::less<>> files;
    bool is_mounted = true;

    bool Initialize() override { return true; }
    bool IsAvailable() const override { return is_mounted; }
    bool WriteFile(std::string_view, const void*, size_t, bool) override { return false; }
    bool ReadFile(std::string_view, void*, size_t, size_t& out_len) override { out_len = 0; return false; }
    int OpenFile(std::string_view, fs_mode_t, fs_file_t*) override { return -ENOTSUP; }
    bool CreateDirectory(std::string_view) override { return false; }
    bool Exists(std::string_view relative_path) const override { return is_mounted && files.contains(relative_path); }
    bool DeleteFile(std::string_view) override { return false; }
    bool DeleteRecursive(std::string_view) override { return false; }
    int Rename(std::string_view, std::string_view) override { return -ENOTSUP; }
    std::vector<std::string> ListFiles(std::string_view) const override { return {}; }

    int ListDirectory(std::string_view, size_t, size_t, std::vector<DirectoryEntry>& entries, size_t& count) const override {
        entries.clear();
        count = 0;

        return -ENOTSUP;
    }

    std::optional<size_t> GetFileSize(std::string_view) const override { return std::nullopt; }
    uint64_t GetTotalSpace() const override { return 0; }
    uint64_t GetUsedSpace() const override { return 0; }
    bool Format() override { return false; }
};

} // namespace eerie_leap::tests
