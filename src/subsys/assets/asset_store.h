#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>

#include <zephyr/fs/fs.h>

#include "subsys/fs/services/i_fs_service.h"

namespace eerie_leap::subsys::assets {

using eerie_leap::subsys::fs::services::IFsService;

class AssetStore;

/** @brief A file of a store, as AssetStore::List() reads it. */
struct AssetEntry {
    std::string name;
    size_t size = 0;
    /// Position in the directory, counting every entry: list on from index + 1.
    size_t index = 0;
};

/**
 * @brief An asset open for reading, from AssetStore::OpenRead(). Move-only; closes on destruction.
 *
 * Reads go through a buffer of BUFFER_SIZE bytes (Mrm::GetExtPmr()) filled at multiples of
 * BUFFER_SIZE, so the file system sees aligned reads of whole sectors whatever offsets the caller
 * asks for. A read that crosses the buffer's end refills it and continues.
 *
 * Reads run on the file handle directly, outside the file system service's mutex. littlefs locks
 * each call per mount; FatFs only with CONFIG_FS_FATFS_REENTRANT (Zephyr's default is n), which a
 * FAT volume needs whenever another thread uses it at the same time, as the log writer does. A
 * reader is used by one thread at a time.
 */
class AssetReader {
public:
    static constexpr size_t BUFFER_SIZE = 4096;

    AssetReader() = default;
    ~AssetReader();
    AssetReader(AssetReader&& other) noexcept;
    AssetReader& operator=(AssetReader&& other) noexcept;
    AssetReader(const AssetReader&) = delete;
    AssetReader& operator=(const AssetReader&) = delete;

    [[nodiscard]] bool IsOpen() const;
    /** @brief The size of the file when it was opened. */
    [[nodiscard]] size_t GetSize() const;

    /**
     * @brief Reads up to @p buffer.size() bytes from @p offset.
     * @return The bytes read (fewer only at the end), 0 at or past the end, or a negative errno
     *         (-EBADF when not open). A failed read leaves the reader open: the caller closes it.
     */
    ssize_t Read(size_t offset, std::span<uint8_t> buffer);

    void Close();

private:
    friend class AssetStore;

    // Aligned for the block device underneath, which may transfer by DMA.
    struct alignas(32) Block {
        uint8_t data[BUFFER_SIZE];
    };

    static constexpr size_t UNKNOWN_POSITION = std::numeric_limits<size_t>::max();

    fs_file_t file_{};
    bool open_ = false;
    size_t size_ = 0;
    eerie_memory::pmr_unique_ptr<Block> buffer_;
    // The file range the buffer holds: starts at a multiple of BUFFER_SIZE, shorter at the end.
    size_t buffer_offset_ = 0;
    size_t buffer_length_ = 0;
    // Where the file position is, so that the next block in sequence is read without a seek.
    size_t file_position_ = 0;

    int Fill(size_t block_offset);
    void MoveFrom(AssetReader& other) noexcept;
};

/**
 * @brief A staged write from AssetStore::BeginWrite(). Move-only; aborts on destruction unless
 *        committed.
 *
 * Appended data collects in a buffer of BUFFER_SIZE bytes (Mrm::GetExtPmr()) written out when it is
 * full, so the staged file is written in aligned blocks.
 *
 * Writes run on the file handle directly, outside the file system service's mutex, and are locked
 * as reads are (see AssetReader): by littlefs per call, by FatFs only with
 * CONFIG_FS_FATFS_REENTRANT. A writer is used by one thread at a time.
 */
class AssetWriter {
public:
    static constexpr size_t BUFFER_SIZE = 4096;

    AssetWriter() = default;
    ~AssetWriter();
    AssetWriter(AssetWriter&& other) noexcept;
    AssetWriter& operator=(AssetWriter&& other) noexcept;
    AssetWriter(const AssetWriter&) = delete;
    AssetWriter& operator=(const AssetWriter&) = delete;

    [[nodiscard]] bool IsOpen() const;
    /** @brief Bytes accepted by Append() so far. */
    [[nodiscard]] size_t GetWritten() const;

    /**
     * @brief Appends @p data to the staged file.
     * @return 0; -ENOSPC when the volume is full (FAT reports that as a short write, which counts
     *         as full); another negative errno on failure. A failure leaves the writer open: the
     *         caller aborts it. It also stays failed: later calls return the same error, and
     *         Commit() refuses to publish a file with a hole in it.
     */
    int Append(std::span<const uint8_t> data);

    /**
     * @brief Writes out the buffer, closes the staged file and renames it onto the asset, creating
     *        the store's directory first when it is missing. Replaces an existing asset.
     * @return 0, or a negative errno, in which case the staged file is deleted. The writer is
     *         closed either way.
     */
    int Commit();

    /** @brief Closes and deletes the staged file. No effect when not open. */
    void Abort();

private:
    friend class AssetStore;

    // Aligned for the block device underneath, which may transfer by DMA.
    struct alignas(32) Block {
        uint8_t data[BUFFER_SIZE];
    };

    std::shared_ptr<IFsService> fs_service_;
    // The store's directory, created by Commit() when it is missing.
    std::string directory_;
    // Where Commit() renames the staged file to, relative to the volume.
    std::string target_path_;
    fs_file_t file_{};
    bool open_ = false;
    // The first failure of a write; Append() and Commit() return it from then on.
    int error_ = 0;
    eerie_memory::pmr_unique_ptr<Block> buffer_;
    size_t buffered_ = 0;
    size_t written_ = 0;

    int Flush();
    int CloseFile();
    void DiscardStaged();
    void MoveFrom(AssetWriter& other) noexcept;
};

/**
 * @brief Assets in one directory of one file system service.
 */
class AssetStore {
public:
    /// Directory of staged writes, at the root of the store's volume.
    static constexpr std::string_view STAGING_DIRECTORY = ".assets_tmp";
    /// The staged file: one write is staged per volume at a time.
    static constexpr std::string_view STAGING_PATH = ".assets_tmp/upload.part";

    /**
     * @param directory Relative to the volume's mount point; empty for its root. It is created now
     *        when the volume is available (as today), otherwise when a write needs it.
     * @throws std::invalid_argument when @p fs_service is null.
     */
    AssetStore(std::shared_ptr<IFsService> fs_service, std::string_view directory);

    // Whole-file access by a path relative to the store's directory. No name rules apply here: a
    // widget's FILE_PATH may hold a sub-path, as it may today.
    bool Save(std::string_view relative_path, std::span<const uint8_t> data);
    /**
     * @brief The whole file on Mrm::GetExtPmr(); empty when it is missing, empty or unreadable, or
     *        when its size changed while it was read (detect it by reading one byte more than the
     *        size and requiring exactly the size). Never throws: whatever goes wrong, including a
     *        file that does not fit the memory, or a size that no buffer can hold, gives an empty
     *        result.
     */
    std::pmr::vector<uint8_t> Load(std::string_view relative_path) const;
    bool Delete(std::string_view relative_path);
    bool Exists(std::string_view relative_path) const;
    /** @return The size of the file; nullopt when it is missing or is no file, a directory say. */
    std::optional<size_t> GetSize(std::string_view relative_path) const;

    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] const std::string& GetDirectory() const {
        return directory_;
    }
    /** @brief @p relative_path relative to the volume: directory + "/" + path, or the path alone
     *         for an empty directory. */
    [[nodiscard]] std::string GetPath(std::string_view relative_path) const;
    /** @brief Free bytes on the store's volume: total minus used, 0 when unavailable. */
    [[nodiscard]] uint64_t GetFreeSpace() const;

    /**
     * @brief Reads a page of the directory.
     *
     * Reads at most @p max_entries entries from directory position @p first, and keeps the files
     * whose names pass IsValidName() for @p extensions. Directories and other names are skipped,
     * but counted in @p scanned.
     *
     * @param entries Cleared, then gets the files kept, each with its directory position.
     * @param scanned Gets the number of directory entries read, kept or not.
     * @param count Gets the number of entries in the directory.
     * @return 0; 0 with nothing listed (and count 0) for a missing directory; -ENODEV when the
     *         volume is unavailable; another negative errno of IFsService::ListDirectory().
     * @throws std::bad_alloc when an entry cannot be stored.
     */
    int List(
        size_t first,
        size_t max_entries,
        std::span<const std::string_view> extensions,
        std::vector<AssetEntry>& entries,
        size_t& scanned,
        size_t& count
    ) const;

    /**
     * @brief Opens @p name (relative to the directory) for reading; a reader that is still open is
     *        closed first.
     * @return 0; -ENODEV when unavailable; -ENOENT when it does not exist or is a directory;
     *         another negative errno (-ENOMEM when the file system has no free file slot, or the
     *         reader's buffer cannot be allocated).
     */
    int OpenRead(std::string_view name, AssetReader& reader) const;

    /**
     * @brief Starts a staged write of @p name: data goes to STAGING_PATH on this store's volume
     *        (created, or truncated when a stale one is there) until AssetWriter::Commit() renames
     *        it onto the asset. A writer that is still open is aborted first.
     * @return 0; -ENODEV when unavailable; another negative errno.
     */
    int BeginWrite(std::string_view name, AssetWriter& writer);

    /** @brief Deletes a staged file left on the volume, e.g. by a power loss during an upload.
     *         @return true when none is left. */
    bool RemoveStaged();

    /**
     * @brief Whether @p name may name an asset of a kind with @p extensions.
     *
     * 1 to CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH bytes of [A-Za-z0-9._-], not starting with '.',
     * ending in one of @p extensions (each given with its dot, compared ASCII case-insensitively)
     * with at least one character before the extension.
     */
    static bool IsValidName(std::string_view name, std::span<const std::string_view> extensions);

private:
    std::shared_ptr<IFsService> fs_service_;
    std::string directory_;
};

} // namespace eerie_leap::subsys::assets
