#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include "dfu_test_images.h"
#include "smp_test_client.h"
#include "subsys/dfu/dfu_status.h"
#include "subsys/dfu/image_identity.h"
#include "subsys/dfu/image_upload_guard.h"
#include "subsys/dfu/mcuboot_headers.h"
#include "subsys/dfu/smp/dfu_mgmt_group.h"

namespace dfu_test {

using eerie_leap::subsys::dfu::DfuState;
using eerie_leap::subsys::dfu::DfuStatus;
using eerie_leap::subsys::dfu::IDfuStatusSource;
using eerie_leap::subsys::dfu::ImageIdentity;
using eerie_leap::subsys::dfu::ImageUploadGuard;
using eerie_leap::subsys::dfu::smp::DfuMgmtGroup;
using eerie_leap::subsys::smp::SmpGroupId;
using smp_test::Bytes;
using smp_test::CborMap;
using smp_test::Response;
using smp_test::SmpTestClient;

// MCUmgr's groups of Zephyr, which SmpGroupId (the application's groups) does not name.
inline constexpr auto IMG_GROUP = static_cast<SmpGroupId>(MGMT_GROUP_ID_IMAGE);
inline constexpr uint8_t IMG_STATE = 0;
inline constexpr uint8_t IMG_UPLOAD = 1;

// DfuError, as the dfu group sends them.
inline constexpr uint32_t ERROR_NO_IDENTITY = 2;
inline constexpr uint32_t ERROR_WRONG_PRODUCT = 3;
inline constexpr uint32_t ERROR_WRONG_BOARD = 4;
inline constexpr uint32_t ERROR_BAD_IDENTITY = 5;

inline constexpr size_t SLOT_SIZE = 0x10000;
inline constexpr size_t CHUNK_SIZE = 512;
inline constexpr size_t SHA_SIZE = 32;

using Image = std::span<const uint8_t>;

// The identity of this unit, which the images are signed for (CMakeLists.txt).
inline ImageIdentity OwnIdentity() {
    const auto identity = ImageIdentity::Create(DFU_TEST_PRODUCT_ID, CONFIG_BOARD_TARGET);
    zassert_true(identity.has_value());

    return *identity;
}

// A source of the dfu group's STATUS that says what a test sets.
class FakeStatusSource : public IDfuStatusSource {
public:
    DfuStatus status;

    [[nodiscard]] DfuStatus GetStatus() const override {
        return status;
    }
};

// The unit: image management of Zephyr, the upload guard and the dfu group, and a client.
struct Unit {
    FakeStatusSource status_source;
    std::optional<ImageUploadGuard> guard;
    std::optional<DfuMgmtGroup> group;
    SmpTestClient client;

    Unit() {
        Start();
    }

    // Registers the guard and the group.
    void Start() {
        guard.emplace(OwnIdentity());
        guard->Register();
        group.emplace(status_source);
        group->Register();
    }
};

// The flash area of an image slot.
inline const flash_area* OpenSlot(uint8_t area_id) {
    const flash_area* area = nullptr;
    zassert_equal(flash_area_open(area_id, &area), 0);

    return area;
}

inline constexpr uint8_t SLOT0_ID = PARTITION_ID(slot0_partition);
inline constexpr uint8_t SLOT1_ID = PARTITION_ID(slot1_partition);

// Erases an image slot, trailer included.
inline void EraseSlot(uint8_t area_id) {
    const flash_area* area = OpenSlot(area_id);
    zassert_equal(flash_area_flatten(area, 0, area->fa_size), 0);
    flash_area_close(area);
}

// Whether the first 32 bytes of the image in slot 1 (where MCUboot's mode puts it) are erased.
inline bool IsSlot1HeaderErased() {
    const flash_area* area = OpenSlot(SLOT1_ID);
    std::array<uint8_t, 32> header{};
    zassert_equal(flash_area_read(area, boot_get_image_start_offset(SLOT1_ID), header.data(), header.size()), 0);
    flash_area_close(area);

    return std::ranges::all_of(header, [](uint8_t byte) {
        return byte == 0xFF;
    });
}

// The SHA-256 of an image file, which the images header names as text.
inline Bytes ParseSha(std::string_view hex) {
    Bytes sha;
    for(size_t i = 0; i + 1 < hex.size(); i += 2) {
        const std::string pair(hex.substr(i, 2));
        sha.push_back(static_cast<uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    zassert_equal(sha.size(), SHA_SIZE);

    return sha;
}

// The offset at which the protected TLVs of an image end: the chunk that holds the byte before it
// completes the identity.
inline size_t ProtectedAreaEnd(Image image) {
    uint16_t header_size = 0;
    uint16_t protected_size = 0;
    uint32_t payload_size = 0;
    std::memcpy(&header_size, image.data() + 8, sizeof(header_size));
    std::memcpy(&protected_size, image.data() + 10, sizeof(protected_size));
    std::memcpy(&payload_size, image.data() + 12, sizeof(payload_size));

    return static_cast<size_t>(header_size) + payload_size + protected_size;
}

// One upload request: the chunk at @p offset; the first one also names the size and the hash.
inline Response SendChunk(
    SmpTestClient& client,
    Image image,
    std::string_view sha,
    size_t offset,
    size_t chunk_size,
    size_t declared_size
) {
    const size_t length = std::min(chunk_size, image.size() - offset);

    CborMap body;
    body.Put("off", static_cast<uint32_t>(offset));
    if(offset == 0) {
        body.Put("len", static_cast<uint32_t>(declared_size));
        body.Put("sha", ParseSha(sha));
    }
    body.Put("data", image.subspan(offset, length));

    return client.Write(IMG_GROUP, IMG_UPLOAD, body.Build());
}

// What an upload came to.
struct UploadOutcome {
    // The offset of the chunk that was refused, or of the last one that was sent.
    size_t refused_at = 0;
    bool is_refused = false;
    Response last;
};

// Sends the image chunk by chunk, from @p start, until it is complete or a chunk gets an error.
inline UploadOutcome
Upload(SmpTestClient& client, Image image, std::string_view sha, size_t chunk_size = CHUNK_SIZE, size_t start = 0) {
    UploadOutcome outcome;

    size_t offset = start;
    while(offset < image.size()) {
        outcome.refused_at = offset;
        outcome.last = SendChunk(client, image, sha, offset, chunk_size, image.size());
        if(!outcome.last.IsOk()) {
            outcome.is_refused = true;
            return outcome;
        }

        // The unit says where it wants the next chunk, which is where this one ended unless it has the data.
        const size_t next = outcome.last.Value("off");
        zassert_true(next > offset || next == image.size(), "The unit asks for offset %zu after %zu", next, offset);
        offset = next;
    }

    return outcome;
}

// The images img_mgmt lists: one entry per slot that holds one.
inline Response ReadImages(SmpTestClient& client) {
    return client.Read(IMG_GROUP, IMG_STATE);
}

} // namespace dfu_test
