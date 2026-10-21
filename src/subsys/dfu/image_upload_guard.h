#pragma once

#include <cstdint>

#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>

#include "image_identity.h"

// Defined by img_mgmt.h (see mcuboot_headers.h), which the .cpp includes: the header needs none of MCUboot's.
struct img_mgmt_upload_check;

namespace eerie_leap::subsys::dfu {

/**
 * @brief Refuses an `img_mgmt` upload whose image is not built for this unit.
 *
 * Registers an MCUmgr callback on MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK, which `img_mgmt` calls for every
 * chunk before it writes it (the state write behind *test* and *confirm* has notifications only, so
 * this is the one hook that can refuse). The chunks go through an ImageIdentityCollector, which
 * compares the identity TLV of the image (ImageIdentity) with the unit's own as soon as the
 * protected TLVs have passed.
 *
 * A refused chunk gets an SMP v2 error in group 72 with the DfuError, and:
 * - `img_mgmt`'s upload state is reset, so that a resume with the same hash starts over;
 * - when an earlier chunk of the same upload was accepted, the flash sector that holds the image
 *   header is erased, so that the partial image can neither be listed nor tested. A refusal of the
 *   first chunk writes nothing, and leaves the slot as it was (it can hold the backup image).
 *
 * The callback runs on the MCUmgr work queue: no allocation, no exceptions. MCUmgr passes no
 * context to callbacks, so only one guard can be registered.
 */
class ImageUploadGuard {
private:
    static inline ImageUploadGuard* instance = nullptr;

    ImageIdentityCollector collector_;
    mgmt_callback callback_{};
    // A chunk of the upload in progress was accepted, so the slot has been written to.
    bool has_written_ = false;

    static mgmt_cb_return OnEvent(
        uint32_t event,
        mgmt_cb_return previous_status,
        int32_t* error_code,
        uint16_t* error_group,
        bool* abort_more,
        void* data,
        size_t data_size
    );

    mgmt_cb_return CheckChunk(const img_mgmt_upload_check& check, int32_t* error_code, uint16_t* error_group);
    /** @brief Erases the sector of slot @p area_id that holds the image header. */
    static void EraseHeaderSector(int area_id);

public:
    /** @param own The identity of this unit. */
    explicit ImageUploadGuard(const ImageIdentity& own);
    ~ImageUploadGuard();

    ImageUploadGuard(const ImageUploadGuard&) = delete;
    ImageUploadGuard& operator=(const ImageUploadGuard&) = delete;
    ImageUploadGuard(ImageUploadGuard&&) = delete;
    ImageUploadGuard& operator=(ImageUploadGuard&&) = delete;

    /** @throws std::logic_error if another guard is registered. */
    void Register();
    void Unregister();

    [[nodiscard]] bool IsRegistered() const {
        return instance == this;
    }
};

} // namespace eerie_leap::subsys::dfu
