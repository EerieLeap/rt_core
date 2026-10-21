#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/dfu/image_identity.h"

// Builds MCUboot image files for the identity tests: header, payload, protected TLVs and a few
// unprotected ones, laid out as imgtool does.
namespace dfu_test {

using eerie_leap::subsys::dfu::ImageIdentity;
using eerie_leap::subsys::dfu::ImageIdentityCollector;
using eerie_leap::subsys::dfu::ImageIdentityResult;

using Bytes = std::vector<uint8_t>;

inline constexpr uint16_t OWN_PRODUCT = 0xA401;
inline constexpr std::string_view OWN_BOARD = "esp32s3_touch_amoled_1_75/esp32s3/procpu";

inline ImageIdentity Identity(uint16_t product, std::string_view board) {
    const auto identity = ImageIdentity::Create(product, board);
    zassert_true(identity.has_value(), "Identity does not fit");

    return *identity;
}

inline ImageIdentity OwnIdentity() {
    return Identity(OWN_PRODUCT, OWN_BOARD);
}

inline void PutU16(Bytes& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

inline void PutU32(Bytes& bytes, uint32_t value) {
    PutU16(bytes, static_cast<uint16_t>(value & 0xFFFF));
    PutU16(bytes, static_cast<uint16_t>(value >> 16));
}

// One TLV: type, length, value.
inline Bytes Tlv(uint16_t type, std::span<const uint8_t> value) {
    Bytes tlv;
    PutU16(tlv, type);
    PutU16(tlv, static_cast<uint16_t>(value.size()));
    tlv.insert(tlv.end(), value.begin(), value.end());

    return tlv;
}

inline Bytes IdentityTlv(const ImageIdentity& identity) {
    Bytes value(ImageIdentity::MAX_ENCODED_SIZE);
    value.resize(identity.Encode(value));

    return Tlv(ImageIdentity::TLV_TYPE, value);
}

// A protected TLV area: the info header (magic, total size) and the TLVs.
inline Bytes ProtectedArea(const std::vector<Bytes>& tlvs) {
    Bytes body;
    for(const Bytes& tlv : tlvs)
        body.insert(body.end(), tlv.begin(), tlv.end());

    Bytes area;
    PutU16(area, ImageIdentityCollector::PROTECTED_TLV_MAGIC);
    PutU16(area, static_cast<uint16_t>(4 + body.size()));
    area.insert(area.end(), body.begin(), body.end());

    return area;
}

struct ImageParts {
    size_t header_size = ImageIdentityCollector::HEADER_SIZE;
    size_t payload_size = 300;
    // The protected TLV area as it lies in the file; empty for an image without one.
    Bytes protected_area;
    // Overrides, to build malformed headers.
    uint32_t magic = ImageIdentityCollector::HEADER_MAGIC;
    std::optional<uint16_t> protected_size_field;
    // Unprotected TLVs (hash, signature) after the protected ones.
    size_t unprotected_size = 100;
};

inline Bytes MakeImage(const ImageParts& parts) {
    Bytes image;
    PutU32(image, parts.magic);
    PutU32(image, 0);
    PutU16(image, static_cast<uint16_t>(parts.header_size));
    PutU16(image, parts.protected_size_field.value_or(static_cast<uint16_t>(parts.protected_area.size())));
    PutU32(image, static_cast<uint32_t>(parts.payload_size));
    // Flags, version and padding up to the 32 bytes of the header.
    image.resize(ImageIdentityCollector::HEADER_SIZE, 0);

    image.resize(parts.header_size, 0);
    for(size_t i = 0; i < parts.payload_size; i++)
        image.push_back(static_cast<uint8_t>(0x55 + i));

    image.insert(image.end(), parts.protected_area.begin(), parts.protected_area.end());
    image.insert(image.end(), parts.unprotected_size, 0xEE);

    return image;
}

// An image with the identity in the protected TLVs.
inline Bytes MakeIdentityImage(const ImageIdentity& identity) {
    ImageParts parts;
    parts.protected_area = ProtectedArea({ IdentityTlv(identity) });

    return MakeImage(parts);
}

// Feeds @p image to @p collector in one chunk each of @p sizes, the rest in the last.
inline ImageIdentityResult
FeedChunks(ImageIdentityCollector& collector, const Bytes& image, std::span<const size_t> sizes) {
    ImageIdentityResult result = ImageIdentityResult::NEED_MORE;

    size_t offset = 0;
    for(size_t i = 0; i <= sizes.size() && offset < image.size(); i++) {
        const size_t size = i < sizes.size() ? sizes[i] : image.size() - offset;
        const size_t length = std::min(size, image.size() - offset);
        result = collector.Feed(offset, std::span(image).subspan(offset, length), image.size());
        offset += length;
    }

    return result;
}

// Feeds @p image in two chunks, cut after @p split bytes.
inline ImageIdentityResult FeedSplit(ImageIdentityCollector& collector, const Bytes& image, size_t split) {
    const size_t sizes[] = { split };

    return FeedChunks(collector, image, sizes);
}

} // namespace dfu_test
