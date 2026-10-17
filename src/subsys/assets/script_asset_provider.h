#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "asset_kind.h"
#include "asset_provider.h"
#include "asset_store.h"

namespace eerie_leap::subsys::assets {

class AssetsManager;

/// The extension of a Lua script, in static storage as AssetProvider::extensions needs.
inline constexpr std::array<std::string_view, 1> LUA_SCRIPT_EXTENSIONS = {".lua"};

/**
 * @brief What a unit serves of a kind of Lua scripts: listing, download, upload and delete of
 *        ".lua" files up to @p max_size bytes.
 *
 * A DELETE of a script that some referrer of @p manager refers to is refused as in use, whichever
 * kind's directory the script is in: a sensor may use a script from the CAN scripts' directory and
 * a CAN message one from the sensors'. Overwriting (WRITE) and reading one is allowed.
 *
 * The provider holds @p manager weakly, since the manager owns the provider.
 *
 * @param name The kind's name; a string literal.
 */
AssetProvider MakeScriptProvider(
    AssetKind kind,
    std::string_view name,
    std::shared_ptr<AssetStore> store,
    uint32_t max_size,
    const std::shared_ptr<AssetsManager>& manager);

} // namespace eerie_leap::subsys::assets
