#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"

namespace eerie_leap::subsys::assets {

using eerie_leap::subsys::fs::services::IFsService;

class AssetsManager {
private:
    std::shared_ptr<IFsService> fs_service_;

    const std::string assets_dir_;

    std::string GetFullPath(std::string_view relative_path) const;

public:
    AssetsManager(std::shared_ptr<IFsService> fs_service, std::string_view assets_dir);

    bool Save(std::string_view relative_path, std::span<const uint8_t> data);
    std::pmr::vector<uint8_t> Load(std::string_view relative_path);
    bool Delete(std::string_view relative_path);

    bool Exists(std::string_view relative_path) const;

    const std::string& GetAssetsDir() const { return assets_dir_; }
};

} // namespace eerie_leap::subsys::assets
