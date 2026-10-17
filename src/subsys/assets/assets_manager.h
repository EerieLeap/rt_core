#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "asset_kind.h"
#include "asset_provider.h"

namespace eerie_leap::subsys::assets {

/**
 * @brief The asset kinds a unit serves, each registered by the controller that owns its feature.
 *
 * Thread-safe: Find() is called under the LVGL lock by widgets and from the configuration work
 * queue by the SMP group. The registry's k_mutex is never held while a provider's is_in_use or a
 * referrer runs.
 *
 * Create it with std::make_shared: it holds a kernel mutex, so it neither copies nor moves.
 */
class AssetsManager {
public:
    /// Tells whether a stored configuration refers to a file, by its path relative to the volume.
    using Referrer = std::function<bool(std::string_view path)>;

    AssetsManager();
    AssetsManager(const AssetsManager&) = delete;
    AssetsManager& operator=(const AssetsManager&) = delete;

    /** @return false, registering nothing, when the kind is registered already or the provider
     *          has no store. */
    bool Register(AssetProvider provider);
    /** @brief Never throws, so a destructor may call it. */
    void Unregister(AssetKind kind) noexcept;
    /** @return The kind's provider; nullptr when none is registered. */
    [[nodiscard]] std::shared_ptr<const AssetProvider> Find(AssetKind kind) const;
    /** @return The registered providers, ordered by kind. */
    [[nodiscard]] std::vector<std::shared_ptr<const AssetProvider>> GetProviders() const;

    /** @brief Adds a referrer for @p owner, any address that identifies it (usually the
     *         registering controller). */
    void AddReferrer(const void* owner, Referrer referrer);
    /**
     * @brief Removes every referrer of @p owner. Never throws, so a destructor may call it: when
     *        there is no memory to hold them aside, they are destroyed under the lock.
     */
    void RemoveReferrers(const void* owner) noexcept;
    /** @brief Whether some referrer refers to @p path. Referrers run without the lock held. */
    [[nodiscard]] bool IsReferenced(std::string_view path) const;

private:
    struct ReferrerEntry {
        const void* owner;
        std::shared_ptr<const Referrer> referrer;
    };

    mutable k_mutex mutex_;
    // By kind. Providers and referrers are shared, so that a caller keeps using the one it found
    // while another thread unregisters it, and so that the lock covers only the lookup.
    std::array<std::shared_ptr<const AssetProvider>, ASSET_KIND_COUNT> providers_;
    std::vector<ReferrerEntry> referrers_;
};

} // namespace eerie_leap::subsys::assets
