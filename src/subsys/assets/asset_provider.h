#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "asset_kind.h"
#include "asset_store.h"

namespace eerie_leap::subsys::assets {

/** @brief What a unit serves of one asset kind. */
struct AssetProvider {
    AssetKind kind{};
    /// The name the kind is listed under ("images"); a string literal.
    std::string_view name;
    std::shared_ptr<AssetStore> store;
    /// Extensions with their dot (".lua"), in static storage: the provider keeps only the span.
    std::span<const std::string_view> extensions;
    /// AssetAccess bits.
    uint8_t access = 0;
    /// Largest upload, in bytes; 0 for a kind that takes no uploads.
    uint32_t max_size = 0;
    /**
     * Whether @p operation on the asset @p name must be refused as in use; empty when nothing ever
     * is. Called on the configuration work queue without the registry's lock. Must not take the
     * LVGL lock.
     */
    std::function<bool(AssetAccess operation, std::string_view name)> is_in_use;
    /**
     * The end of the asset @p name in milliseconds since the Unix epoch (UTC), for a kind whose
     * files have one the listing cannot read off the file (the `logs` kind); null when not known.
     * Empty: the kind lists no end. Called once per listed entry on the configuration work queue
     * without the registry's lock: no file I/O, no waiting. Must not take the LVGL lock.
     */
    std::function<std::optional<int64_t>(std::string_view name)> get_end_ms;
};

} // namespace eerie_leap::subsys::assets
