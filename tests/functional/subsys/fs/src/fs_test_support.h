#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

namespace fs_test {

using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;

/*
 * Shared instance for the suites that only need a mounted volume; the service itself never owns
 * the devicetree mount entry, so tests are free to create their own instances as well.
 */
inline FsService* GetFsService() {
    static FsService* fs_service = nullptr;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        fs_service = new FsService(DtFs::GetInternalFsMp());
        fs_service->Initialize();
    }

    return fs_service;
}

inline void ResetFs(void* = nullptr) {
    GetFsService()->DeleteRecursive();
}

inline bool WriteText(std::string_view relative_path, std::string_view content, bool append = false) {
    return GetFsService()->WriteFile(relative_path, content.data(), content.size(), append);
}

inline std::string ReadText(std::string_view relative_path) {
    auto* fs_service = GetFsService();

    std::vector<char> buffer(fs_service->GetFileSize(relative_path).value_or(0) + 1);
    size_t read_size = 0;

    if(!fs_service->ReadFile(relative_path, buffer.data(), buffer.size(), read_size))
        return {};

    return {buffer.data(), read_size};
}

inline std::string MakeText(size_t size) {
    std::string content(size, '\0');

    for(size_t i = 0; i < size; ++i)
        content[i] = static_cast<char>('a' + (i % 26));

    return content;
}

} // namespace fs_test
