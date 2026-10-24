#include <algorithm>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "assets_manager.h"

namespace eerie_leap::subsys::assets {

LOG_MODULE_REGISTER(assets_manager_logger);

using eerie_leap::subsys::threading::ScopedMutex;

AssetsManager::AssetsManager() {
    k_mutex_init(&mutex_);
}

bool AssetsManager::Register(AssetProvider provider) {
    const size_t index = std::to_underlying(provider.kind);

    if(index >= ASSET_KIND_COUNT) {
        LOG_ERR("Asset kind %d is out of range.", static_cast<int>(index));

        return false;
    }

    if(provider.store == nullptr) {
        LOG_ERR("Asset kind %d has no store.", static_cast<int>(index));

        return false;
    }

    // A built-in is served under a name the kind could store, once.
    for(size_t i = 0; i < provider.built_ins.size(); i++) {
        const std::string_view built_in_name = provider.built_ins[i].name;

        if(!AssetStore::IsValidName(built_in_name, provider.extensions)) {
            LOG_ERR("Asset kind %d has a built-in with an invalid name.", static_cast<int>(index));

            return false;
        }

        // The first of the equal names is the one that is found; a second is a mistake.
        if(FindBuiltIn(provider, built_in_name) != &provider.built_ins[i]) {
            LOG_ERR("Asset kind %d has a built-in name twice.", static_cast<int>(index));

            return false;
        }
    }

    // Allocated before the lock is taken.
    std::shared_ptr<const AssetProvider> shared = std::make_shared<AssetProvider>(std::move(provider));

    ScopedMutex lock(mutex_);

    if(providers_[index] != nullptr) {
        LOG_ERR("Asset kind %d is registered already.", static_cast<int>(index));

        return false;
    }

    providers_[index] = std::move(shared);

    return true;
}

void AssetsManager::Unregister(AssetKind kind) noexcept {
    const size_t index = std::to_underlying(kind);

    if(index >= ASSET_KIND_COUNT)
        return;

    // Declared before the lock, so that it is released after it: the provider dies here when
    // nobody else holds it, and what it captured is destroyed without the lock.
    std::shared_ptr<const AssetProvider> removed;

    ScopedMutex lock(mutex_);

    removed = std::move(providers_[index]);
}

std::shared_ptr<const AssetProvider> AssetsManager::Find(AssetKind kind) const {
    const size_t index = std::to_underlying(kind);

    if(index >= ASSET_KIND_COUNT)
        return nullptr;

    ScopedMutex lock(mutex_);

    return providers_[index];
}

std::vector<std::shared_ptr<const AssetProvider>> AssetsManager::GetProviders() const {
    std::vector<std::shared_ptr<const AssetProvider>> providers;
    providers.reserve(ASSET_KIND_COUNT);

    ScopedMutex lock(mutex_);

    for(const auto& provider : providers_) {
        if(provider != nullptr)
            providers.push_back(provider);
    }

    return providers;
}

void AssetsManager::AddReferrer(const void* owner, Referrer referrer) {
    // A call through an empty function would throw.
    if(!referrer)
        return;

    // Allocated before the lock is taken.
    std::shared_ptr<const Referrer> shared = std::make_shared<Referrer>(std::move(referrer));

    ScopedMutex lock(mutex_);

    referrers_.push_back({ owner, std::move(shared) });
}

void AssetsManager::RemoveReferrers(const void* owner) noexcept {
    // Declared before the lock, so that the referrers are released after it: destroying one
    // destroys what it captured, which is not for the registry's lock to cover.
    std::vector<std::shared_ptr<const Referrer>> removed;

    ScopedMutex lock(mutex_);

    // Never throws out of here: controllers call this from their destructors. Without room for the
    // list the referrers are destroyed under the lock instead, which only costs the promise above.
    bool is_release_deferred = true;
    try {
        removed.reserve(referrers_.size());
    } catch(...) {
        is_release_deferred = false;
    }

    // The room is there, so the pushes below cannot allocate.
    if(is_release_deferred) {
        for(auto& entry : referrers_) {
            if(entry.owner == owner)
                removed.push_back(std::move(entry.referrer));
        }
    }

    std::erase_if(referrers_, [owner](const ReferrerEntry& entry) {
        return entry.owner == owner;
    });
}

bool AssetsManager::IsReferenced(std::string_view path) const {
    std::vector<std::shared_ptr<const Referrer>> referrers;

    {
        ScopedMutex lock(mutex_);

        referrers.reserve(referrers_.size());

        for(const auto& entry : referrers_)
            referrers.push_back(entry.referrer);
    }

    // A referrer may call back into the registry, and may be removed meanwhile: the copies keep
    // it alive, and no lock is held.
    return std::ranges::any_of(referrers, [path](const std::shared_ptr<const Referrer>& referrer) {
        return (*referrer)(path);
    });
}

} // namespace eerie_leap::subsys::assets
