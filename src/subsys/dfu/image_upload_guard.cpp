#include <span>
#include <stdexcept>
#include <utility>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include "subsys/smp/smp_group_id.h"

#include "dfu_error.h"
#include "image_upload_guard.h"
#include "mcuboot_headers.h"

LOG_MODULE_REGISTER(image_upload_guard_logger, CONFIG_EERIE_LEAP_DFU_LOG_LEVEL);

namespace eerie_leap::subsys::dfu {

using eerie_leap::subsys::smp::SmpGroupId;

namespace {

DfuError ToDfuError(ImageIdentityResult result) {
    switch(result) {
        case ImageIdentityResult::NO_IDENTITY:
            return DfuError::NO_IDENTITY;
        case ImageIdentityResult::WRONG_PRODUCT:
            return DfuError::WRONG_PRODUCT;
        case ImageIdentityResult::WRONG_BOARD:
            return DfuError::WRONG_BOARD;
        case ImageIdentityResult::BAD_IDENTITY:
            return DfuError::BAD_IDENTITY;
        case ImageIdentityResult::NEED_MORE:
        case ImageIdentityResult::MATCH:
            break;
    }

    return DfuError::UNKNOWN;
}

const char* ToText(ImageIdentityResult result) {
    switch(result) {
        case ImageIdentityResult::NO_IDENTITY:
            return "no identity";
        case ImageIdentityResult::WRONG_PRODUCT:
            return "wrong product";
        case ImageIdentityResult::WRONG_BOARD:
            return "wrong board";
        case ImageIdentityResult::BAD_IDENTITY:
            return "bad identity";
        case ImageIdentityResult::NEED_MORE:
        case ImageIdentityResult::MATCH:
            break;
    }

    return "accepted";
}

} // namespace

ImageUploadGuard::ImageUploadGuard(const ImageIdentity& own) :
    collector_(own) {
    callback_.callback = OnEvent;
    callback_.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK;
}

ImageUploadGuard::~ImageUploadGuard() {
    Unregister();
}

void ImageUploadGuard::Register() {
    if(instance == this)
        return;

    if(instance != nullptr)
        throw std::logic_error("Another image upload guard is registered");

    instance = this;
    collector_.Reset();
    has_written_ = false;
    mgmt_callback_register(&callback_);
}

void ImageUploadGuard::Unregister() {
    if(instance != this)
        return;

    mgmt_callback_unregister(&callback_);
    instance = nullptr;
}

mgmt_cb_return ImageUploadGuard::OnEvent(
    uint32_t event,
    mgmt_cb_return previous_status,
    int32_t* error_code,
    uint16_t* error_group,
    bool* /*abort_more*/,
    void* data,
    size_t data_size
) {
    ImageUploadGuard* guard = instance;
    if(guard == nullptr
        || event != MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK
        || data == nullptr
        || data_size != sizeof(img_mgmt_upload_check))
        return MGMT_CB_OK;

    // Another callback refused the chunk, so img_mgmt does not write it and the client sends it again.
    if(previous_status != MGMT_CB_OK)
        return MGMT_CB_OK;

    return guard->CheckChunk(*static_cast<const img_mgmt_upload_check*>(data), error_code, error_group);
}

mgmt_cb_return
ImageUploadGuard::CheckChunk(const img_mgmt_upload_check& check, int32_t* error_code, uint16_t* error_group) {
    const img_mgmt_upload_req& request = *check.req;

    // The first chunk starts an upload, whatever was in progress.
    if(request.off == 0)
        has_written_ = false;

    const std::span<const uint8_t> chunk(
        request.img_data.len > 0 ? request.img_data.value : nullptr, request.img_data.len
    );
    const ImageIdentityResult result = collector_.Feed(request.off, chunk, static_cast<size_t>(check.action->size));

    if(result == ImageIdentityResult::NEED_MORE || result == ImageIdentityResult::MATCH) {
        has_written_ = has_written_ || !chunk.empty();
        return MGMT_CB_OK;
    }

    LOG_WRN("Image refused at offset %zu: %s", request.off, ToText(result));

    if(has_written_)
        EraseHeaderSector(check.action->area_id);

    // The partial image is gone or was never written: a resume with the same hash must not continue it.
    img_mgmt_reset_upload();
    has_written_ = false;

    *error_group = std::to_underlying(SmpGroupId::DFU);
    *error_code = std::to_underlying(ToDfuError(result));

    return MGMT_CB_ERROR_ERR;
}

void ImageUploadGuard::EraseHeaderSector(int area_id) {
    const flash_area* area = nullptr;
    if(area_id < 0 || flash_area_open(static_cast<uint8_t>(area_id), &area) != 0) {
        LOG_ERR("Slot %d cannot be opened to erase the refused image", area_id);
        return;
    }

    // With swap-using-offset MCUboot's secondary slot keeps its image one sector in.
    const off_t header_offset = static_cast<off_t>(
        area->fa_off + boot_get_image_start_offset(static_cast<uint8_t>(area_id))
    );

    flash_pages_info page{};
    const device* flash = flash_area_get_device(area);
    int rc = -ENODEV;
    if(flash != nullptr)
        rc = flash_get_page_info_by_offs(flash, header_offset, &page);

    if(rc == 0)
        rc = flash_area_flatten(area, page.start_offset - static_cast<off_t>(area->fa_off), page.size);

    if(rc != 0)
        LOG_ERR("Header sector of slot %d not erased: %d", area_id, rc);

    flash_area_close(area);
}

} // namespace eerie_leap::subsys::dfu
