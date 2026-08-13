#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/fs/fs.h>

namespace eerie_leap::subsys::fs::services {

class IFsService {
public:
    virtual ~IFsService() = default;

    virtual bool Initialize() = 0;
    virtual bool IsAvailable() const = 0;
    virtual bool WriteFile(std::string_view relative_path, const void* data_p, size_t data_size, bool append) = 0;
    virtual bool ReadFile(std::string_view relative_path, void* data_p, size_t data_size, size_t& out_len) = 0;

    /** @return 0 on success, or the negative errno reported by fs_open(). */
    virtual int OpenFile(std::string_view relative_path, fs_mode_t flags, fs_file_t* file_p) = 0;

    virtual bool CreateDirectory(std::string_view relative_path) = 0;
    virtual bool Exists(std::string_view relative_path) const = 0;
    virtual bool DeleteFile(std::string_view relative_path) = 0;
    virtual bool DeleteRecursive(std::string_view relative_path) = 0;
    virtual std::vector<std::string> ListFiles(std::string_view relative_path) const = 0;
    virtual std::optional<size_t> GetFileSize(std::string_view relative_path) const = 0;
    virtual uint64_t GetTotalSpace() const = 0;
    virtual uint64_t GetUsedSpace() const = 0;
    virtual bool Format() = 0;

    // Non-virtual shorthands: default arguments on virtuals bind to the static type.
    bool WriteFile(std::string_view relative_path, const void* data_p, size_t data_size) {
        return WriteFile(relative_path, data_p, data_size, false);
    }

    bool DeleteRecursive() { return DeleteRecursive(std::string_view{}); }

    std::vector<std::string> ListFiles() const { return ListFiles(std::string_view{}); }
};

} // namespace eerie_leap::subsys::fs::services
