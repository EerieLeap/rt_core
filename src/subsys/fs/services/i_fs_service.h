#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/fs/fs.h>

namespace eerie_leap::subsys::fs::services {

/** @brief An entry of a directory, as IFsService::ListDirectory() reads it. */
struct DirectoryEntry {
    std::string name;
    /// In bytes; 0 for a directory.
    size_t size = 0;
    bool is_directory = false;
};

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

    /**
     * @brief Reads a directory: how many entries it holds, and those from its @p first one.
     *
     * Entries come in the file system's order, without "." and "..". Paging by @p first holds
     * together while the directory does not change between calls.
     *
     * @param relative_path The directory; empty for the root of the volume.
     * @param first Index of the first entry kept.
     * @param max_entries Entries kept at most; 0 only counts them.
     * @param entries Cleared, then gets the entries kept.
     * @param count Gets the number of entries in the directory.
     * @return 0; -ENODEV when not mounted; -EINVAL for a path that is absolute, has a ".." segment
     *         or does not fit the path buffer; -ENOTDIR for a file; otherwise the negative errno of
     *         fs_opendir() or fs_readdir(), -ENOENT for no such directory. On an error @p entries
     *         is empty and @p count is 0.
     * @throws std::bad_alloc when an entry cannot be stored.
     */
    virtual int ListDirectory(
        std::string_view relative_path,
        size_t first,
        size_t max_entries,
        std::vector<DirectoryEntry>& entries,
        size_t& count) const = 0;

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
