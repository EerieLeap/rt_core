#include <utility>

#include "assets_manager.h"

#include "script_asset_provider.h"

namespace eerie_leap::subsys::assets {

AssetProvider MakeScriptProvider(
    AssetKind kind,
    std::string_view name,
    std::shared_ptr<AssetStore> store,
    uint32_t max_size,
    const std::shared_ptr<AssetsManager>& manager) {

    AssetProvider provider;
    provider.kind = kind;
    provider.name = name;
    provider.extensions = LUA_SCRIPT_EXTENSIONS;
    provider.access = ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE) | ToMask(AssetAccess::DELETE);
    provider.max_size = max_size;

    // The store is kept for the path of a name; it does not refer to the manager, the registry's
    // owner, so the provider and the store do not form a cycle.
    provider.is_in_use = [weak_manager = std::weak_ptr<AssetsManager>(manager), store](
        AssetAccess operation, std::string_view asset_name) {

        if(operation != AssetAccess::DELETE)
            return false;

        const auto locked_manager = weak_manager.lock();

        return locked_manager != nullptr && locked_manager->IsReferenced(store->GetPath(asset_name));
    };

    provider.store = std::move(store);

    return provider;
}

} // namespace eerie_leap::subsys::assets
