#include <algorithm>

#include "image_identity.h"

namespace eerie_leap::subsys::dfu {

namespace {

// Offsets of the fields of MCUboot's `struct image_header` (little-endian).
constexpr size_t HEADER_MAGIC_OFFSET = 0;
constexpr size_t HEADER_SIZE_OFFSET = 8;
constexpr size_t HEADER_PROTECTED_TLV_SIZE_OFFSET = 10;
constexpr size_t HEADER_IMAGE_SIZE_OFFSET = 12;

// `struct image_tlv_info` ahead of the TLVs of an area, and `struct image_tlv` ahead of each value.
constexpr size_t TLV_INFO_SIZE = 4;
constexpr size_t TLV_HEADER_SIZE = 4;

uint16_t ReadU16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(ReadU16(bytes, offset)) | (static_cast<uint32_t>(ReadU16(bytes, offset + 2)) << 16);
}

} // namespace

std::optional<ImageIdentity> ImageIdentity::Parse(std::span<const uint8_t> value) {
    if(value.size() < FIXED_SIZE || value[0] != FORMAT)
        return std::nullopt;

    const size_t board_length = value[3];
    if(value.size() != FIXED_SIZE + board_length)
        return std::nullopt;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the board is raw bytes of the TLV
    const std::string_view board(reinterpret_cast<const char*>(value.data()) + FIXED_SIZE, board_length);

    return Create(ReadU16(value, 1), board);
}

size_t ImageIdentity::Encode(std::span<uint8_t> out) const {
    const size_t size = FIXED_SIZE + board_length_;
    if(out.size() < size)
        return 0;

    out[0] = FORMAT;
    out[1] = static_cast<uint8_t>(product_id_ & 0xFF);
    out[2] = static_cast<uint8_t>(product_id_ >> 8);
    out[3] = board_length_;
    std::copy_n(board_.begin(), board_length_, out.begin() + FIXED_SIZE);

    return size;
}

ImageIdentityResult ImageIdentity::Check(const ImageIdentity& image) const {
    if(image.product_id_ != product_id_)
        return ImageIdentityResult::WRONG_PRODUCT;

    if(image.GetBoard() != GetBoard())
        return ImageIdentityResult::WRONG_BOARD;

    return ImageIdentityResult::MATCH;
}

ImageIdentityCollector::ImageIdentityCollector(const ImageIdentity& own) :
    own_(own) {}

void ImageIdentityCollector::Reset() {
    phase_ = Phase::HEADER;
    result_ = ImageIdentityResult::NEED_MORE;
    received_ = 0;
    window_start_ = 0;
    window_size_ = HEADER_SIZE;
}

ImageIdentityResult ImageIdentityCollector::Decide(ImageIdentityResult result) {
    phase_ = Phase::DECIDED;
    result_ = result;

    return result;
}

void ImageIdentityCollector::CopyToWindow(size_t offset, std::span<const uint8_t> chunk) {
    const size_t from = std::max(offset, window_start_);
    const size_t to = std::min(offset + chunk.size(), window_start_ + window_size_);
    if(from >= to)
        return;

    std::copy_n(
        chunk.begin() + static_cast<std::ptrdiff_t>(from - offset), to - from, window_.begin() + (from - window_start_)
    );
}

bool ImageIdentityCollector::IsWindowComplete() const {
    return received_ >= window_start_ + window_size_;
}

ImageIdentityResult ImageIdentityCollector::ParseHeader(size_t image_size) {
    const std::span<const uint8_t> header(window_.data(), HEADER_SIZE);

    if(ReadU32(header, HEADER_MAGIC_OFFSET) != HEADER_MAGIC)
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    const size_t header_size = ReadU16(header, HEADER_SIZE_OFFSET);
    const size_t protected_size = ReadU16(header, HEADER_PROTECTED_TLV_SIZE_OFFSET);
    const uint64_t payload_size = ReadU32(header, HEADER_IMAGE_SIZE_OFFSET);

    if(header_size < HEADER_SIZE)
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    if(protected_size == 0)
        return Decide(ImageIdentityResult::NO_IDENTITY);

    if(protected_size > MAX_PROTECTED_TLV_SIZE)
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    const uint64_t area_start = header_size + payload_size;
    if(area_start + protected_size > image_size)
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    phase_ = Phase::TLVS;
    window_start_ = static_cast<size_t>(area_start);
    window_size_ = protected_size;

    return ImageIdentityResult::NEED_MORE;
}

ImageIdentityResult ImageIdentityCollector::ParseTlvs() {
    const std::span<const uint8_t> area(window_.data(), window_size_);

    if(window_size_ < TLV_INFO_SIZE || ReadU16(area, 0) != PROTECTED_TLV_MAGIC || ReadU16(area, 2) != window_size_)
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    std::optional<ImageIdentity> identity;
    size_t position = TLV_INFO_SIZE;
    while(position < window_size_) {
        if(window_size_ - position < TLV_HEADER_SIZE)
            return Decide(ImageIdentityResult::BAD_IDENTITY);

        const uint16_t type = ReadU16(area, position);
        const size_t length = ReadU16(area, position + 2);
        position += TLV_HEADER_SIZE;

        if(length > window_size_ - position)
            return Decide(ImageIdentityResult::BAD_IDENTITY);

        if(type == ImageIdentity::TLV_TYPE) {
            // A second identity would leave open which one counts.
            if(identity.has_value())
                return Decide(ImageIdentityResult::BAD_IDENTITY);

            identity = ImageIdentity::Parse(area.subspan(position, length));
            if(!identity.has_value())
                return Decide(ImageIdentityResult::BAD_IDENTITY);
        }

        position += length;
    }

    if(!identity.has_value())
        return Decide(ImageIdentityResult::NO_IDENTITY);

    return Decide(own_.Check(*identity));
}

ImageIdentityResult ImageIdentityCollector::Feed(size_t offset, std::span<const uint8_t> chunk, size_t image_size) {
    if(offset == 0)
        Reset();

    if(phase_ == Phase::DECIDED)
        return result_;

    // A chunk that skips data leaves the TLV bytes unknown.
    if(offset > received_ || chunk.size() > image_size - std::min(offset, image_size))
        return Decide(ImageIdentityResult::BAD_IDENTITY);

    received_ = std::max(received_, offset + chunk.size());

    CopyToWindow(offset, chunk);
    if(phase_ == Phase::HEADER && IsWindowComplete() && ParseHeader(image_size) == ImageIdentityResult::NEED_MORE)
        CopyToWindow(offset, chunk);

    if(phase_ == Phase::TLVS && IsWindowComplete())
        return ParseTlvs();

    return result_;
}

} // namespace eerie_leap::subsys::dfu
