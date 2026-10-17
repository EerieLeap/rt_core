#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace eerie_leap::subsys::assets {

/**
 * @brief The kinds of assets a unit can serve.
 *
 * The values go over the wire (the SMP assets group): append only, never reorder or remove.
 */
enum class AssetKind : uint8_t {
    IMAGES = 0,
    ANIMATIONS = 1,
    SENSOR_SCRIPTS = 2,
    CAN_SCRIPTS = 3,
    LOGS = 4,
};

/// Number of AssetKind values.
inline constexpr size_t ASSET_KIND_COUNT = 5;

// A kind appended without raising the count would be refused by the registry as out of range.
static_assert(ASSET_KIND_COUNT == static_cast<size_t>(std::to_underlying(AssetKind::LOGS)) + 1,
    "ASSET_KIND_COUNT must follow the last AssetKind");

/** @brief Operations on a kind's assets; as bits, the `access` mask of the SMP assets group. */
enum class AssetAccess : uint8_t {
    READ = 1 << 0,
    WRITE = 1 << 1,
    DELETE = 1 << 2,
};

constexpr uint8_t ToMask(AssetAccess access) { return std::to_underlying(access); }
constexpr bool HasAccess(uint8_t mask, AssetAccess access) { return (mask & ToMask(access)) != 0; }

} // namespace eerie_leap::subsys::assets
