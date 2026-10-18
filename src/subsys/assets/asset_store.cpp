#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "asset_store.h"

namespace eerie_leap::subsys::assets {

LOG_MODULE_REGISTER(asset_store_logger);

using namespace eerie_leap::utilities::memory;

using eerie_leap::subsys::fs::services::DirectoryEntry;

namespace {

// Creates the directory unless it exists; an empty one is the volume's root.
bool EnsureDirectory(IFsService& fs_service, std::string_view directory) {
    if(directory.empty() || fs_service.Exists(directory))
        return true;

    return fs_service.CreateDirectory(directory);
}

constexpr char ToLowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Anything above 0x7F is negative as a char, so UTF-8 is refused here too.
constexpr bool IsNameCharacter(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '.'
        || c == '_'
        || c == '-';
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    if(text.size() < suffix.size())
        return false;

    const size_t start = text.size() - suffix.size();

    for(size_t i = 0; i < suffix.size(); i++) {
        if(ToLowerAscii(text[start + i]) != ToLowerAscii(suffix[i]))
            return false;
    }

    return true;
}

} // namespace

// ======== AssetReader ========

AssetReader::~AssetReader() {
    Close();
}

AssetReader::AssetReader(AssetReader&& other) noexcept {
    MoveFrom(other);
}

AssetReader& AssetReader::operator=(AssetReader&& other) noexcept {
    if(this != &other) {
        Close();
        MoveFrom(other);
    }

    return *this;
}

void AssetReader::MoveFrom(AssetReader& other) noexcept {
    // The handle names its file by pointers into the file system's own tables, so a copy of it
    // is the same handle.
    file_ = other.file_;
    open_ = other.open_;
    size_ = other.size_;
    buffer_ = std::move(other.buffer_);
    buffer_offset_ = other.buffer_offset_;
    buffer_length_ = other.buffer_length_;
    file_position_ = other.file_position_;

    other.file_ = {};
    other.open_ = false;
    other.size_ = 0;
    other.buffer_offset_ = 0;
    other.buffer_length_ = 0;
    other.file_position_ = 0;
}

bool AssetReader::IsOpen() const {
    return open_;
}

size_t AssetReader::GetSize() const {
    return size_;
}

void AssetReader::Close() {
    if(!open_)
        return;

    // The file system releases the handle whether or not it reports an error, so it is never
    // closed twice.
    fs_close(&file_);
    file_ = {};

    open_ = false;
    size_ = 0;
    buffer_.reset();
    buffer_offset_ = 0;
    buffer_length_ = 0;
    file_position_ = 0;
}

ssize_t AssetReader::Read(size_t offset, std::span<uint8_t> buffer) {
    if(!open_)
        return -EBADF;

    if(buffer.empty() || offset >= size_)
        return 0;

    const size_t length = std::min(buffer.size(), size_ - offset);
    size_t copied = 0;

    while(copied < length) {
        const size_t position = offset + copied;

        if(position < buffer_offset_ || position >= buffer_offset_ + buffer_length_) {
            const int rc = Fill(position - position % BUFFER_SIZE);
            if(rc < 0)
                return rc;

            // The file is shorter than when it was opened: this is its end.
            if(position >= buffer_offset_ + buffer_length_)
                break;
        }

        const size_t in_buffer = position - buffer_offset_;
        const size_t chunk = std::min(length - copied, buffer_length_ - in_buffer);

        std::memcpy(buffer.data() + copied, buffer_->data + in_buffer, chunk);
        copied += chunk;
    }

    return static_cast<ssize_t>(copied);
}

int AssetReader::Fill(size_t block_offset) {
    buffer_offset_ = block_offset;
    buffer_length_ = 0;

    // A block that follows the one read before it needs no seek. A seek back on FAT walks the
    // cluster chain from the start of the file.
    if(file_position_ != block_offset) {
        file_position_ = UNKNOWN_POSITION;

        const int rc = fs_seek(&file_, static_cast<off_t>(block_offset), FS_SEEK_SET);
        if(rc < 0)
            return rc;

        file_position_ = block_offset;
    }

    const size_t expected = std::min(BUFFER_SIZE, size_ - block_offset);
    size_t filled = 0;

    while(filled < expected) {
        const ssize_t rc = fs_read(&file_, buffer_->data + filled, expected - filled);
        if(rc < 0) {
            file_position_ = UNKNOWN_POSITION;

            return static_cast<int>(rc);
        }

        // The file got shorter since it was opened.
        if(rc == 0)
            break;

        filled += static_cast<size_t>(rc);
        file_position_ += static_cast<size_t>(rc);
    }

    buffer_length_ = filled;

    return 0;
}

// ======== AssetWriter ========

AssetWriter::~AssetWriter() {
    Abort();
}

AssetWriter::AssetWriter(AssetWriter&& other) noexcept {
    MoveFrom(other);
}

AssetWriter& AssetWriter::operator=(AssetWriter&& other) noexcept {
    if(this != &other) {
        Abort();
        MoveFrom(other);
    }

    return *this;
}

void AssetWriter::MoveFrom(AssetWriter& other) noexcept {
    fs_service_ = std::move(other.fs_service_);
    directory_ = std::move(other.directory_);
    target_path_ = std::move(other.target_path_);
    file_ = other.file_;
    open_ = other.open_;
    error_ = other.error_;
    buffer_ = std::move(other.buffer_);
    buffered_ = other.buffered_;
    written_ = other.written_;

    other.file_ = {};
    other.open_ = false;
    other.error_ = 0;
    other.buffered_ = 0;
    other.written_ = 0;
}

bool AssetWriter::IsOpen() const {
    return open_;
}

size_t AssetWriter::GetWritten() const {
    return written_;
}

int AssetWriter::Append(std::span<const uint8_t> data) {
    if(!open_)
        return -EBADF;

    if(error_ != 0)
        return error_;

    size_t offset = 0;

    while(offset < data.size()) {
        const size_t length = std::min(data.size() - offset, BUFFER_SIZE - buffered_);

        std::memcpy(buffer_->data + buffered_, data.data() + offset, length);
        buffered_ += length;
        offset += length;
        written_ += length;

        if(buffered_ == BUFFER_SIZE) {
            const int rc = Flush();
            if(rc != 0)
                return rc;
        }
    }

    return 0;
}

int AssetWriter::Flush() {
    if(buffered_ == 0)
        return 0;

    const ssize_t rc = fs_write(&file_, buffer_->data, buffered_);

    if(rc < 0) {
        error_ = static_cast<int>(rc);
    } else if(static_cast<size_t>(rc) != buffered_) {
        // FAT reports a full volume as a short write, with no error.
        LOG_WRN("Staged write stopped short: %d of %d bytes.", static_cast<int>(rc), static_cast<int>(buffered_));
        error_ = -ENOSPC;
    } else {
        buffered_ = 0;
    }

    return error_;
}

int AssetWriter::CloseFile() {
    // The file system releases the handle whether or not it reports an error, so it is never
    // closed twice.
    const int rc = fs_close(&file_);
    file_ = {};
    open_ = false;

    return rc;
}

void AssetWriter::DiscardStaged() {
    if(fs_service_ != nullptr && fs_service_->Exists(AssetStore::STAGING_PATH))
        fs_service_->DeleteFile(AssetStore::STAGING_PATH);
}

int AssetWriter::Commit() {
    if(!open_)
        return -EBADF;

    int rc = error_ != 0 ? error_ : Flush();

    // A write-back that fails shows on close.
    const int close_rc = CloseFile();
    if(rc == 0)
        rc = close_rc;

    if(rc == 0 && !EnsureDirectory(*fs_service_, directory_)) {
        LOG_ERR("Failed to create the asset directory.");
        rc = -EIO;
    }

    if(rc == 0)
        rc = fs_service_->Rename(AssetStore::STAGING_PATH, target_path_);

    if(rc != 0)
        DiscardStaged();

    buffer_.reset();
    buffered_ = 0;

    return rc;
}

void AssetWriter::Abort() {
    if(!open_)
        return;

    CloseFile();
    DiscardStaged();

    buffer_.reset();
    buffered_ = 0;
    error_ = 0;
}

// ======== AssetStore ========

AssetStore::AssetStore(std::shared_ptr<IFsService> fs_service, std::string_view directory) :
    fs_service_(std::move(fs_service)),
    directory_(directory) {

    if(fs_service_ == nullptr)
        throw std::invalid_argument("fs_service cannot be null");

    if(fs_service_->IsAvailable())
        EnsureDirectory(*fs_service_, directory_);
}

std::string AssetStore::GetPath(std::string_view relative_path) const {
    if(directory_.empty())
        return std::string(relative_path);

    std::string path;
    path.reserve(directory_.size() + 1 + relative_path.size());
    path.append(directory_);
    path.push_back('/');
    path.append(relative_path);

    return path;
}

bool AssetStore::IsAvailable() const {
    return fs_service_->IsAvailable();
}

bool AssetStore::Save(std::string_view relative_path, std::span<const uint8_t> data) {
    if(!fs_service_->IsAvailable())
        return false;

    // The volume may have come up after the store was created.
    if(!EnsureDirectory(*fs_service_, directory_))
        return false;

    return fs_service_->WriteFile(GetPath(relative_path), data.data(), data.size(), false);
}

std::pmr::vector<uint8_t> AssetStore::Load(std::string_view relative_path) const {
    if(!fs_service_->IsAvailable())
        return {};

    try {
        const std::string path = GetPath(relative_path);

        const size_t file_size = fs_service_->GetFileSize(path).value_or(0);
        if(file_size == 0)
            return {};

        // One byte more than the file holds: reading it shows that the file grew since its size
        // was read, and a short read that it shrank.
        std::pmr::vector<uint8_t> buffer(file_size + 1, Mrm::GetExtPmr());
        size_t out_len = 0;

        if(!fs_service_->ReadFile(path, buffer.data(), buffer.size(), out_len) || out_len != file_size)
            return {};

        buffer.resize(file_size);

        return buffer;
    } catch(const std::bad_alloc&) {
        LOG_ERR("Not enough memory to load an asset.");

        return {};
    } catch(const std::exception& e) {
        // A size that no vector can hold (std::length_error), say, which is what the size of
        // something that is no file can come to.
        LOG_ERR("Failed to load an asset: %s.", e.what());

        return {};
    }
}

bool AssetStore::Delete(std::string_view relative_path) {
    if(!fs_service_->IsAvailable())
        return false;

    return fs_service_->DeleteFile(GetPath(relative_path));
}

bool AssetStore::Exists(std::string_view relative_path) const {
    if(!fs_service_->IsAvailable())
        return false;

    return fs_service_->Exists(GetPath(relative_path));
}

std::optional<size_t> AssetStore::GetSize(std::string_view relative_path) const {
    if(!fs_service_->IsAvailable())
        return std::nullopt;

    return fs_service_->GetFileSize(GetPath(relative_path));
}

uint64_t AssetStore::GetFreeSpace() const {
    if(!fs_service_->IsAvailable())
        return 0;

    const uint64_t total = fs_service_->GetTotalSpace();
    const uint64_t used = fs_service_->GetUsedSpace();

    return total > used ? total - used : 0;
}

int AssetStore::List(
    size_t first,
    size_t max_entries,
    std::span<const std::string_view> extensions,
    std::vector<AssetEntry>& entries,
    size_t& scanned,
    size_t& count
) const {

    entries.clear();
    scanned = 0;
    count = 0;

    // A volume can be unavailable while it is still mounted, an SD card pulled while its monitor
    // is off, so the answer is not left to ListDirectory().
    if(!fs_service_->IsAvailable())
        return -ENODEV;

    std::vector<DirectoryEntry> directory_entries;
    const int rc = fs_service_->ListDirectory(directory_, first, max_entries, directory_entries, count);

    // A directory that was never created holds nothing.
    if(rc == -ENOENT) {
        count = 0;

        return 0;
    }

    if(rc < 0) {
        count = 0;

        return rc;
    }

    scanned = directory_entries.size();

    for(size_t i = 0; i < directory_entries.size(); i++) {
        auto& entry = directory_entries[i];

        if(entry.is_directory || !IsValidName(entry.name, extensions))
            continue;

        entries.push_back(
            {
                .name = std::move(entry.name),
                .size = entry.size,
                .index = first + i,
            }
        );
    }

    return 0;
}

int AssetStore::OpenRead(std::string_view name, AssetReader& reader) const {
    // A handle that is still open must not be handed to OpenFile(), which would forget it and leak
    // its file slot for good.
    reader.Close();

    if(!fs_service_->IsAvailable())
        return -ENODEV;

    if(name.empty())
        return -ENOENT;

    // Everything that can throw comes before the file is opened.
    eerie_memory::pmr_unique_ptr<AssetReader::Block> buffer;
    std::string path;

    try {
        buffer = eerie_memory::make_unique_pmr<AssetReader::Block>(Mrm::GetExtPmr());
        path = GetPath(name);
    } catch(const std::bad_alloc&) {
        LOG_ERR("Not enough memory to open an asset.");

        return -ENOMEM;
    }

    fs_file_t file{};

    int rc = fs_service_->OpenFile(path, FS_O_READ, &file);

    // A directory is no file to read; FAT says no such file, littlefs says it is a directory.
    if(rc == -EISDIR)
        rc = -ENOENT;

    if(rc < 0)
        return rc;

    // The size comes from the open file, so that it and the content belong to the same file.
    off_t size = 0;
    rc = fs_seek(&file, 0, FS_SEEK_END);

    if(rc == 0) {
        size = fs_tell(&file);
        rc = size < 0 ? static_cast<int>(size) : fs_seek(&file, 0, FS_SEEK_SET);
    }

    if(rc < 0) {
        fs_close(&file);

        return rc;
    }

    reader.file_ = file;
    reader.open_ = true;
    reader.size_ = static_cast<size_t>(size);
    reader.buffer_ = std::move(buffer);
    reader.buffer_offset_ = 0;
    reader.buffer_length_ = 0;
    reader.file_position_ = 0;

    return 0;
}

int AssetStore::BeginWrite(std::string_view name, AssetWriter& writer) {
    // A writer that is still open owns the staged file and a file slot; both go first.
    writer.Abort();

    if(!fs_service_->IsAvailable())
        return -ENODEV;

    if(name.empty())
        return -EINVAL;

    // Everything that can throw comes before the file is opened.
    eerie_memory::pmr_unique_ptr<AssetWriter::Block> buffer;
    std::string directory;
    std::string target_path;

    try {
        buffer = eerie_memory::make_unique_pmr<AssetWriter::Block>(Mrm::GetExtPmr());
        directory = directory_;
        target_path = GetPath(name);
    } catch(const std::bad_alloc&) {
        LOG_ERR("Not enough memory to write an asset.");

        return -ENOMEM;
    }

    if(!EnsureDirectory(*fs_service_, STAGING_DIRECTORY)) {
        LOG_ERR("Failed to create the staging directory.");

        return -EIO;
    }

    fs_file_t file{};

    const int rc = fs_service_->OpenFile(STAGING_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC, &file);
    if(rc < 0)
        return rc;

    writer.fs_service_ = fs_service_;
    writer.directory_ = std::move(directory);
    writer.target_path_ = std::move(target_path);
    writer.file_ = file;
    writer.open_ = true;
    writer.error_ = 0;
    writer.buffer_ = std::move(buffer);
    writer.buffered_ = 0;
    writer.written_ = 0;

    return 0;
}

bool AssetStore::RemoveStaged() {
    if(!fs_service_->IsAvailable())
        return false;

    if(!fs_service_->Exists(STAGING_PATH))
        return true;

    return fs_service_->DeleteFile(STAGING_PATH);
}

bool AssetStore::IsValidName(std::string_view name, std::span<const std::string_view> extensions) {
    if(name.empty() || name.size() > CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH || name.front() == '.')
        return false;

    for(const char c : name) {
        if(!IsNameCharacter(c))
            return false;
    }

    for(const std::string_view extension : extensions) {
        // At least one character stays in front of the extension.
        if(name.size() > extension.size() && EndsWithIgnoreCase(name, extension))
            return true;
    }

    return false;
}

} // namespace eerie_leap::subsys::assets
