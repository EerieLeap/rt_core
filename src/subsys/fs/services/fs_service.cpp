#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "fs_service.h"

namespace eerie_leap::subsys::fs::services {

LOG_MODULE_REGISTER(fs_service_logger);

using eerie_leap::subsys::threading::ScopedMutex;

FsService::FsService(fs_mount_t* mountpoint) :
    mountpoint_(mountpoint) {
    k_mutex_init(&mutex_);
}

bool FsService::TryBuildPath(std::string_view relative_path, FilesystemPath<PATH_BUFFER_SIZE>& full_path) const {
    if(mountpoint_ == nullptr)
        return false;

    // The path is joined onto the mount point, so anything that could escape it is rejected.
    if(!relative_path.empty() && relative_path.front() == '/') {
        LOG_ERR("Absolute paths are not allowed.");
        return false;
    }

    for(size_t start = 0; start <= relative_path.size();) {
        const size_t end = std::min(relative_path.find('/', start), relative_path.size());

        if(relative_path.substr(start, end - start) == "..") {
            LOG_ERR("Parent directory references are not allowed.");
            return false;
        }

        start = end + 1;
    }

    const size_t mount_length = std::strlen(mountpoint_->mnt_point);
    if(mount_length + 1 + relative_path.size() > PATH_BUFFER_SIZE) {
        LOG_ERR("Path does not fit the %d byte path buffer.", static_cast<int>(PATH_BUFFER_SIZE));
        return false;
    }

    full_path = FilesystemPath<PATH_BUFFER_SIZE>(mountpoint_->mnt_point);
    full_path /= relative_path;

    return true;
}

bool FsService::Initialize() {
    ScopedMutex lock(mutex_);

    if(mountpoint_ == nullptr) {
        LOG_ERR("No mount point configured.");
        return false;
    }

    if(!IsMounted())
        return Mount();

    return true;
}

bool FsService::IsAvailable() const {
    ScopedMutex lock(mutex_);

    return IsMounted();
}

bool FsService::WriteFile(std::string_view relative_path, const void* data_p, size_t data_size, bool append) {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    struct fs_file_t file{};
    fs_file_t_init(&file);

    const int rc = fs_open(
        &file, full_path.String().CStr(), FS_O_WRITE | FS_O_CREATE | (append ? FS_O_APPEND : FS_O_TRUNC)
    );
    if(rc < 0) {
        LOG_ERR("fs_open failed: %d.", rc);
        return false;
    }

    const ssize_t written = fs_write(&file, data_p, data_size);
    const int close_rc = fs_close(&file);

    if(written < 0) {
        LOG_ERR("fs_write failed: %d.", static_cast<int>(written));
        return false;
    }

    if(std::cmp_not_equal(written, data_size)) {
        LOG_ERR("fs_write stored %d of %d bytes.", static_cast<int>(written), static_cast<int>(data_size));
        return false;
    }

    if(close_rc < 0) {
        LOG_ERR("fs_close failed: %d.", close_rc);
        return false;
    }

    return true;
}

bool FsService::ReadFile(std::string_view relative_path, void* data_p, size_t data_size, size_t& out_len) {
    ScopedMutex lock(mutex_);

    out_len = 0;

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    struct fs_file_t file{};
    fs_file_t_init(&file);

    const int rc = fs_open(&file, full_path.String().CStr(), FS_O_READ);
    if(rc < 0) {
        LOG_ERR("fs_open failed: %d.", rc);
        return false;
    }

    const ssize_t read = fs_read(&file, data_p, data_size);
    fs_close(&file);

    if(read < 0) {
        LOG_ERR("fs_read failed: %d.", static_cast<int>(read));
        return false;
    }

    out_len = static_cast<size_t>(read);

    return true;
}

int FsService::OpenFile(std::string_view relative_path, fs_mode_t flags, fs_file_t* file_p) {
    ScopedMutex lock(mutex_);

    if(file_p == nullptr)
        return -EINVAL;

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return -ENODEV;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return -EINVAL;

    fs_file_t_init(file_p);

    const int rc = fs_open(file_p, full_path.String().CStr(), flags);
    if(rc < 0)
        LOG_ERR("fs_open failed: %d.", rc);

    return rc;
}

bool FsService::CreateDirectory(std::string_view relative_path) {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    FilesystemPath<PATH_BUFFER_SIZE> current_path(mountpoint_->mnt_point);

    for(size_t start = 0; start < relative_path.size();) {
        const size_t end = std::min(relative_path.find('/', start), relative_path.size());
        const std::string_view segment = relative_path.substr(start, end - start);
        start = end + 1;

        if(segment.empty())
            continue;

        current_path /= segment;

        const int rc = fs_mkdir(current_path.String().CStr());
        if(rc < 0 && rc != -EEXIST) {
            LOG_ERR("Failed to create dir '%s': %d.", current_path.String().CStr(), rc);
            return false;
        }
    }

    return true;
}

bool FsService::Exists(std::string_view relative_path) const {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    struct fs_dirent entry{};
    const int rc = fs_stat(full_path.String().CStr(), &entry);

    return rc == 0;
}

bool FsService::DeleteFile(std::string_view relative_path) {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    const int rc = fs_unlink(full_path.String().CStr());
    if(rc < 0) {
        LOG_ERR("fs_unlink failed: %d.", rc);
        return false;
    }

    return true;
}

bool FsService::DeleteRecursive(std::string_view relative_path) {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return false;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return false;

    return DeleteDirectoryContents(&full_path);
}

// NOLINTNEXTLINE(misc-no-recursion): the depth is bounded by the path buffer, a name is at least two bytes
bool FsService::DeleteDirectoryContents(FilesystemPath<PATH_BUFFER_SIZE>* full_path) {
    struct fs_dir_t dir{};
    struct fs_dirent entry{};
    fs_dir_t_init(&dir);

    int rc = fs_opendir(&dir, full_path->String().CStr());
    if(rc < 0) {
        LOG_ERR("fs_opendir failed on path: %s (%d).", full_path->String().CStr(), rc);
        return false;
    }

    while(true) {
        rc = fs_readdir(&dir, &entry);
        if(rc < 0) {
            LOG_ERR("fs_readdir failed on path: %s (%d).", full_path->String().CStr(), rc);
            fs_closedir(&dir);
            return false;
        }

        if(entry.name[0] == '\0')
            break;

        const size_t base_path_size = full_path->String().Size();
        if(base_path_size + 1 + std::strlen(entry.name) > PATH_BUFFER_SIZE) {
            LOG_ERR("Path does not fit the %d byte path buffer.", static_cast<int>(PATH_BUFFER_SIZE));
            fs_closedir(&dir);
            return false;
        }

        *full_path /= entry.name;

        if(entry.type == FS_DIR_ENTRY_FILE) {
            rc = fs_unlink(full_path->String().CStr());
            if(rc < 0) {
                LOG_ERR("Failed to delete file: %s (%d).", full_path->String().CStr(), rc);
                fs_closedir(&dir);
                return false;
            }
        } else if(entry.type == FS_DIR_ENTRY_DIR) {
            if(!DeleteDirectoryContents(full_path)) {
                fs_closedir(&dir);
                return false;
            }

            // Remove the directory after its contents are gone
            rc = fs_unlink(full_path->String().CStr());
            if(rc < 0) {
                LOG_ERR("Failed to delete dir: %s (%d).", full_path->String().CStr(), rc);
                fs_closedir(&dir);
                return false;
            }
        }

        full_path->String().Truncate(base_path_size);
    }

    fs_closedir(&dir);

    return true;
}

namespace {

// The next name of a path. Empty names ("a//b", a trailing slash) and "." name nothing and are
// skipped; the result is empty once the path is used up.
std::string_view NextName(std::string_view& path) {
    while(!path.empty()) {
        const size_t end = std::min(path.find('/'), path.size());
        const std::string_view name = path.substr(0, end);
        path.remove_prefix(std::min(end + 1, path.size()));

        if(!name.empty() && name != ".")
            return name;
    }

    return {};
}

char ToLowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether two relative paths name the same entry on a volume that folds ASCII case, as FAT does.
bool IsSameEntryIgnoringCase(std::string_view a, std::string_view b) {
    while(true) {
        const std::string_view name_a = NextName(a);
        const std::string_view name_b = NextName(b);

        if(!std::ranges::equal(name_a, name_b, [](char x, char y) {
               return ToLowerAscii(x) == ToLowerAscii(y);
           }))
            return false;

        if(name_a.empty())
            return true;
    }
}

bool IsPlainCharacter(char c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '.'
        || c == '_'
        || c == '-';
}

// Whether a path is spelled so that FAT and littlefs resolve each of its names as it is written:
// only the characters of IsPlainCharacter() and '/', and no name that ends in a dot but "." itself.
// FatFs also reads a backslash as a separator, drops the trailing dots and spaces of a name and
// ends a path at a control byte, which makes more spellings of one file than a comparison can know.
bool IsPlainPath(std::string_view path) {
    for(std::string_view name = NextName(path); !name.empty(); name = NextName(path)) {
        if(name.back() == '.' || !std::ranges::all_of(name, IsPlainCharacter))
            return false;
    }

    return true;
}

} // namespace

int FsService::Rename(std::string_view from, std::string_view to) {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return -ENODEV;
    }

    // An empty path is the root of the volume, which can neither be renamed nor be replaced.
    if(from.empty() || to.empty()) {
        LOG_ERR("Empty paths are not allowed.");
        return -EINVAL;
    }

    FilesystemPath<PATH_BUFFER_SIZE> from_path;
    FilesystemPath<PATH_BUFFER_SIZE> to_path;
    if(!TryBuildPath(from, from_path) || !TryBuildPath(to, to_path))
        return -EINVAL;

    // FatFs folds more spellings onto one name than the comparison below can know, so only plain
    // ones go on.
    if(!IsPlainPath(from) || !IsPlainPath(to)) {
        LOG_ERR("Paths may hold only A-Z a-z 0-9 . _ - and /, and no name may end in a dot.");
        return -EINVAL;
    }

    // FAT unlinks an existing target before it renames, so a file renamed onto itself, or onto its
    // name in another case, would be deleted.
    if(IsSameEntryIgnoringCase(from, to)) {
        LOG_ERR("Source and target are the same file.");
        return -EINVAL;
    }

    // FAT unlinks the target before it looks for the source as well: a missing source would cost
    // the file it was meant to replace.
    struct fs_dirent entry{};
    int rc = fs_stat(from_path.String().CStr(), &entry);
    if(rc < 0) {
        LOG_ERR("fs_stat failed on path: %s (%d).", from_path.String().CStr(), rc);
        return rc;
    }

    rc = fs_rename(from_path.String().CStr(), to_path.String().CStr());
    if(rc < 0)
        LOG_ERR("fs_rename failed: %d.", rc);

    return rc;
}

std::vector<std::string> FsService::ListFiles(std::string_view relative_path) const {
    ScopedMutex lock(mutex_);

    std::vector<std::string> files;

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return files;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return files;

    struct fs_dir_t dir{};
    struct fs_dirent entry{};
    fs_dir_t_init(&dir);

    int rc = fs_opendir(&dir, full_path.String().CStr());
    if(rc < 0) {
        LOG_ERR("fs_opendir failed on path: %s (%d).", full_path.String().CStr(), rc);
        return files;
    }

    while(true) {
        rc = fs_readdir(&dir, &entry);
        if(rc < 0) {
            LOG_ERR("fs_readdir failed on path: %s (%d).", full_path.String().CStr(), rc);
            files.clear();
            break;
        }

        if(entry.name[0] == '\0')
            break;

        files.emplace_back(entry.name);
    }

    fs_closedir(&dir);

    return files;
}

int FsService::ListDirectory(
    std::string_view relative_path,
    size_t first,
    size_t max_entries,
    std::vector<DirectoryEntry>& entries,
    size_t& count
) const {

    ScopedMutex lock(mutex_);

    entries.clear();
    count = 0;

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return -ENODEV;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return -EINVAL;

    struct fs_dir_t dir{};
    struct fs_dirent entry{};
    fs_dir_t_init(&dir);

    int rc = fs_opendir(&dir, full_path.String().CStr());
    if(rc < 0) {
        // FAT takes a file for a missing directory; littlefs answers -ENOTDIR itself.
        if(rc == -ENOENT && fs_stat(full_path.String().CStr(), &entry) == 0 && entry.type == FS_DIR_ENTRY_FILE)
            return -ENOTDIR;

        LOG_ERR("fs_opendir failed on path: %s (%d).", full_path.String().CStr(), rc);
        return rc;
    }

    // Storing an entry may throw, and the directory has to be closed however the walk ends.
    struct DirectoryCloser {
        fs_dir_t* dir;

        explicit DirectoryCloser(fs_dir_t* directory) :
            dir(directory) {}
        DirectoryCloser(const DirectoryCloser&) = delete;
        DirectoryCloser& operator=(const DirectoryCloser&) = delete;
        DirectoryCloser(DirectoryCloser&&) = delete;
        DirectoryCloser& operator=(DirectoryCloser&&) = delete;
        ~DirectoryCloser() {
            fs_closedir(dir);
        }
    };
    const DirectoryCloser closer{ &dir };

    while(true) {
        rc = fs_readdir(&dir, &entry);
        if(rc < 0) {
            LOG_ERR("fs_readdir failed on path: %s (%d).", full_path.String().CStr(), rc);
            entries.clear();
            count = 0;

            return rc;
        }

        if(entry.name[0] == '\0')
            break;

        if(count >= first && entries.size() < max_entries) {
            const bool is_directory = entry.type == FS_DIR_ENTRY_DIR;
            entries.push_back(
                {
                    .name = entry.name,
                    .size = is_directory ? 0 : entry.size,
                    .is_directory = is_directory,
                }
            );
        }

        count++;
    }

    return 0;
}

std::optional<size_t> FsService::GetFileSize(std::string_view relative_path) const {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return std::nullopt;
    }

    FilesystemPath<PATH_BUFFER_SIZE> full_path;
    if(!TryBuildPath(relative_path, full_path))
        return std::nullopt;

    struct fs_dirent entry{};
    const int rc = fs_stat(full_path.String().CStr(), &entry);

    if(rc < 0) {
        LOG_ERR("fs_stat failed: %d.", rc);
        return std::nullopt;
    }

    // The size of a directory is no file size: littlefs leaves it undefined, FAT stores 0.
    if(entry.type == FS_DIR_ENTRY_DIR) {
        LOG_ERR("Path is a directory: %s.", full_path.String().CStr());
        return std::nullopt;
    }

    return entry.size;
}

uint64_t FsService::GetTotalSpace() const {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return 0;
    }

    struct fs_statvfs stat{};
    if(fs_statvfs(mountpoint_->mnt_point, &stat) < 0) {
        LOG_ERR("fs_statvfs failed when querying total space.");
        return 0;
    }

    // f_blocks counts allocation units (f_frsize: a FAT cluster, a littlefs block); f_bsize is only
    // the optimal transfer size.
    return static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
}

uint64_t FsService::GetUsedSpace() const {
    ScopedMutex lock(mutex_);

    if(!IsMounted()) {
        LOG_ERR("Filesystem not mounted.");
        return 0;
    }

    struct fs_statvfs stat{};
    if(fs_statvfs(mountpoint_->mnt_point, &stat) < 0) {
        LOG_ERR("fs_statvfs failed when querying used space.");
        return 0;
    }

    // In allocation units, as in GetTotalSpace().
    return static_cast<uint64_t>(stat.f_blocks - stat.f_bfree) * stat.f_frsize;
}

bool FsService::Format() {
    ScopedMutex lock(mutex_);

    if(mountpoint_ == nullptr) {
        LOG_ERR("No mount point configured.");
        return false;
    }

    Unmount();

    if(IsMounted()) {
        LOG_ERR("Refusing to format a mounted File System.");
        return false;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): fs_mkfs takes the storage device as an integer
    const int rc = fs_mkfs(mountpoint_->type, reinterpret_cast<uintptr_t>(mountpoint_->storage_dev), nullptr, 0);
    if(rc < 0) {
        LOG_ERR("Format failed: %d.", rc);
        Mount();
        return false;
    }

    LOG_INF("Successfully formatted File System.");

    return Mount();
}

uint32_t FsService::GetMountGeneration() const {
    return mount_generation_;
}

bool FsService::Mount() {
    ScopedMutex lock(mutex_);

    if(IsMounted())
        return true;

    if(mountpoint_ == nullptr)
        return false;

    const int rc = fs_mount(mountpoint_);
    if(rc < 0) {
        LOG_ERR("Failed to mount FS: %d.", rc);
        return false;
    }

    mount_generation_++;

    LOG_INF("Mounted FS at %s.", mountpoint_->mnt_point);

    return IsMounted();
}

void FsService::Unmount() {
    ScopedMutex lock(mutex_);

    if(!IsMounted())
        return;

    const int rc = fs_unmount(mountpoint_);
    if(rc < 0)
        LOG_ERR("Failed to unmount FS: %d.", rc);
    else
        LOG_INF("Unmounted FS from %s.", mountpoint_->mnt_point);
}

bool FsService::IsMounted() const {
    return mountpoint_ != nullptr && sys_dnode_is_linked(&mountpoint_->node);
}

} // namespace eerie_leap::subsys::fs::services
