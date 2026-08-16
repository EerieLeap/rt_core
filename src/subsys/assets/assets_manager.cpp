#include "utilities/memory/memory_resource_manager.h"

#include "assets_manager.h"

namespace eerie_leap::subsys::assets {

using namespace eerie_leap::utilities::memory;

AssetsManager::AssetsManager(std::shared_ptr<IFsService> fs_service, std::string_view assets_dir)
    : fs_service_(std::move(fs_service)), assets_dir_(assets_dir) {

    if(!fs_service_->IsAvailable())
        return;

    if(assets_dir_.empty())
        return;

    if(!fs_service_->Exists(assets_dir_))
        fs_service_->CreateDirectory(assets_dir_);
}

std::string AssetsManager::GetFullPath(std::string_view relative_path) const {
    if(assets_dir_.empty())
        return std::string(relative_path);

    return assets_dir_ + "/" + std::string(relative_path);
}

bool AssetsManager::Save(std::string_view relative_path, std::span<const uint8_t> data) {
    if(!fs_service_->IsAvailable())
        return false;

    return fs_service_->WriteFile(GetFullPath(relative_path), data.data(), data.size(), false);
}

std::pmr::vector<uint8_t> AssetsManager::Load(std::string_view relative_path) {
    if(!fs_service_->IsAvailable())
        return {};

    std::string full_path = GetFullPath(relative_path);

    size_t file_size = fs_service_->GetFileSize(full_path).value_or(0);
    if(file_size == 0) {
        return {};
    }

    auto buffer = std::pmr::vector<uint8_t>(file_size, Mrm::GetExtPmr());
    size_t out_len = 0;

    if(!fs_service_->ReadFile(full_path, buffer.data(), file_size, out_len) || out_len == 0) {
        return {};
    }

    return buffer;
}

bool AssetsManager::Delete(std::string_view relative_path) {
    if(!fs_service_->IsAvailable())
        return false;

    return fs_service_->DeleteFile(GetFullPath(relative_path));
}

bool AssetsManager::Exists(std::string_view relative_path) const {
    if(!fs_service_->IsAvailable())
        return false;

    return fs_service_->Exists(GetFullPath(relative_path));
}

} // namespace eerie_leap::subsys::assets
