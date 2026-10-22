#pragma once

#include <cstdint>

namespace eerie_leap::subsys::dfu {

/**
 * @brief Error codes of the dfu SMP group (72), returned in the SMP v2 `err` map. Append only.
 *
 * They answer an `img_mgmt` upload that the unit refuses because of the image's identity
 * (ImageUploadGuard), so a client can tell a wrong image from a failed transfer.
 */
enum class DfuError : uint16_t {
    OK = 0,
    UNKNOWN = 1,
    /// The image carries no identity TLV (a foreign image, or one built before the firmware update).
    NO_IDENTITY = 2,
    /// The image is built for another product.
    WRONG_PRODUCT = 3,
    /// The image is built for another board of this product.
    WRONG_BOARD = 4,
    /// The image's header or its protected TLVs are malformed, or the identity TLV is.
    BAD_IDENTITY = 5,
};

} // namespace eerie_leap::subsys::dfu
