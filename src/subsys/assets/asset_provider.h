#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "utilities/string/ascii_case.h"

#include "asset_kind.h"
#include "asset_store.h"

namespace eerie_leap::subsys::assets {

/// The most bytes of an upload's head that AssetProvider::check_content may ask for.
inline constexpr uint8_t MAX_CONTENT_HEAD_SIZE = 32;

/** @brief The facts an image file's header gives: LVGL's numbers, with no LVGL types. */
struct AssetImageInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    /// The LVGL colour format.
    uint8_t format = 0;
    /// The LVGL compression method; 0 for none.
    uint8_t compression = 0;
};

/** @brief What a kind of self-describing image files says of them; empty for every other kind. */
struct AssetImageTraits {
    /// The LVGL colour formats the kind's image files may be in (KINDS `fmts`), in static storage.
    std::span<const uint32_t> formats;
    /// The LVGL compression methods they may use (KINDS `cmps`), in static storage.
    std::span<const uint32_t> compressions;
    /**
     * The facts of the file @p name, from its head; nullopt for a file of the kind that is not such an
     * image (a raw `.bin`, an `.svg`) or whose head does not read. LIST asks it once per listed stored
     * file, on the configuration work queue without the registry's lock. It may read the file's first
     * bytes; it must not take the LVGL lock.
     */
    std::function<std::optional<AssetImageInfo>(std::string_view name)> get_info;
};

/**
 * @brief A file of a kind that is part of the firmware: read in place from flash, never stored on the
 *        volume, never replaced, renamed or deleted.
 */
struct BuiltInAsset {
    /// A valid name for the kind (AssetStore::IsValidName()); a string literal.
    std::string_view name;
    /// The file's bytes, in static storage (flash): the group reads them in place and keeps no copy.
    std::span<const uint8_t> data;
};

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
    /**
     * The size of the asset @p name that another writer has on the volume, in bytes, for a file that
     * grows while it is open (the log being written): the size as of the writer's last sync, which is
     * what a second handle of the file reads. Null when the asset does not grow now. Called on the
     * configuration work queue without the registry's lock, for every request about the asset: no
     * file I/O, no waiting. Must not take the LVGL lock. Empty for a kind whose files are written
     * only through its store.
     */
    std::function<std::optional<uint64_t>(std::string_view name)> get_growing_size;
    /**
     * The kind's built-in files, in static storage: the provider keeps only the span. The group
     * lists them before the stored files (each entry with `builtin: true`), serves READ and STAT of
     * them from the span and refuses WRITE, DELETE and RENAME of their names with BUILT_IN. A built-in
     * wins over a stored file of the same name (ASCII case ignored): the stored one is not listed and
     * not served. Empty: the kind has none.
     */
    std::span<const BuiltInAsset> built_ins;
    /**
     * The pixel format of the kind's raw pixel files, as the number of an LVGL colour format
     * (0x14 RGB565A8, 0x15 AL88), for a kind whose files carry no header to tell it; the SMP assets
     * group lists it as `fmt`. 0: not a raw pixel kind, nothing is listed.
     */
    uint32_t pixel_format = 0;
    /// What the kind's self-describing image files are; empty for any other kind.
    AssetImageTraits image;
    /// How many of an upload's first bytes check_content needs, at most MAX_CONTENT_HEAD_SIZE; 0 with no check.
    uint8_t content_head_size = 0;
    /**
     * Whether an upload of @p name, @p length bytes long, may be stored, judged from its first
     * min(length, content_head_size) bytes: nullopt to go on, else why not, a sentence the group
     * sends as `msg` with BAD_CONTENT. Called at most once per upload, on the configuration work queue
     * without the registry's lock: no file I/O, no waiting. Must not take the LVGL lock. Empty: the
     * kind takes any content.
     */
    std::function<std::optional<std::string>(std::string_view name, size_t length, std::span<const uint8_t> head)>
        check_content;
};

/** @return The built-in of @p provider that @p name names, ASCII case ignored as names compare on FAT; null when none. */
[[nodiscard]] inline const BuiltInAsset* FindBuiltIn(const AssetProvider& provider, std::string_view name) {
    for(const BuiltInAsset& built_in : provider.built_ins) {
        if(eerie_leap::utilities::string::EqualsIgnoreAsciiCase(built_in.name, name))
            return &built_in;
    }

    return nullptr;
}

} // namespace eerie_leap::subsys::assets
