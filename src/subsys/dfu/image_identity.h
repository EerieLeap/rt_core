#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace eerie_leap::subsys::dfu {

/** @brief What the identity check of an image in transfer has come to. */
enum class ImageIdentityResult : uint8_t {
    /// The protected TLVs are not complete yet.
    NEED_MORE,
    /// The image is built for this product and board.
    MATCH,
    /// No protected TLVs, or none of them is the identity.
    NO_IDENTITY,
    WRONG_PRODUCT,
    WRONG_BOARD,
    /// The header or the protected TLVs are malformed, the identity TLV is, or the image is too short for them.
    BAD_IDENTITY,
};

/**
 * @brief The product and the board a firmware image is built for.
 *
 * The signing step adds it to the image as a protected (signed) MCUboot TLV of type TLV_TYPE, with
 * the value `[format u8 = 1][product_id u16 LE][board_length u8][board ASCII]`. The board is the
 * Zephyr board target (CONFIG_BOARD_TARGET), because one product can have several boards.
 *
 * Plain data without Zephyr dependencies, so that it can run on the MCUmgr work queue: it allocates
 * nothing and throws nothing.
 */
class ImageIdentity {
public:
    /// MCUboot TLV type (imgtool's vendor range) of the identity.
    static constexpr uint16_t TLV_TYPE = 0x00A0;
    static constexpr uint8_t FORMAT = 1;
    /// Format, product ID and board length, ahead of the board.
    static constexpr size_t FIXED_SIZE = 4;
    /// The longest board target: the identity alone in the 128-byte protected TLV area, next to the
    /// area's info header and the TLV's own header (4 bytes each). The same limit as cmake/image_identity.cmake's.
    static constexpr size_t MAX_BOARD_LENGTH = 116;
    static constexpr size_t MAX_ENCODED_SIZE = FIXED_SIZE + MAX_BOARD_LENGTH;

private:
    uint16_t product_id_ = 0;
    uint8_t board_length_ = 0;
    std::array<char, MAX_BOARD_LENGTH> board_{};

    constexpr ImageIdentity() = default;

public:
    /** @return The identity, or nothing when @p board is longer than MAX_BOARD_LENGTH or not printable ASCII. */
    static constexpr std::optional<ImageIdentity> Create(uint16_t product_id, std::string_view board) {
        if(board.size() > MAX_BOARD_LENGTH)
            return std::nullopt;

        ImageIdentity identity;
        identity.product_id_ = product_id;
        identity.board_length_ = static_cast<uint8_t>(board.size());
        for(size_t i = 0; i < board.size(); i++) {
            if(board[i] < ' ' || board[i] > '~')
                return std::nullopt;

            identity.board_.at(i) = board[i];
        }

        return identity;
    }

    /** @brief Decodes the value of an identity TLV; nothing when its format, length or characters are wrong. */
    [[nodiscard]] static std::optional<ImageIdentity> Parse(std::span<const uint8_t> value);

    /**
     * @brief Writes the value of an identity TLV.
     * @return The number of bytes written, 0 when @p out is too small.
     */
    [[nodiscard]] size_t Encode(std::span<uint8_t> out) const;

    [[nodiscard]] constexpr uint16_t GetProductId() const {
        return product_id_;
    }

    [[nodiscard]] constexpr std::string_view GetBoard() const {
        return { board_.data(), board_length_ };
    }

    /**
     * @brief Compares the identity of an image with this one, the unit's own.
     * @return MATCH, WRONG_PRODUCT or WRONG_BOARD, the product checked first.
     */
    [[nodiscard]] ImageIdentityResult Check(const ImageIdentity& image) const;

    [[nodiscard]] constexpr bool operator==(const ImageIdentity& other) const {
        return product_id_ == other.product_id_ && GetBoard() == other.GetBoard();
    }
};

/**
 * @brief Finds the identity in an MCUboot image while it is uploaded, chunk by chunk.
 *
 * An image is `[header][payload][protected TLVs][unprotected TLVs]`. The 32-byte header names the
 * size of the payload and of the protected TLV area, which is where the identity is. The collector
 * keeps a fixed 128-byte window: first over the header, then over the protected TLV area, as the
 * chunks pass (a chunk may hold any part of either, so a window can span chunks). Once the area is
 * complete it is parsed and the identity compared with the unit's own.
 *
 * It decides as early as the image allows: an image without protected TLVs, or with more than
 * MAX_PROTECTED_TLV_SIZE of them, or whose declared size cannot hold them, is refused with its
 * first chunk.
 *
 * Chunks are expected in order. A chunk sent again is harmless, one that skips data fails the
 * check (BAD_IDENTITY). A chunk at offset 0 starts over. The decision is kept until then.
 *
 * Plain data without Zephyr dependencies: it allocates nothing and throws nothing.
 */
class ImageIdentityCollector {
public:
    static constexpr size_t HEADER_SIZE = 32;
    static constexpr size_t MAX_PROTECTED_TLV_SIZE = 128;
    /// `ih_magic` of the MCUboot image header.
    static constexpr uint32_t HEADER_MAGIC = 0x96F3B83D;
    /// `it_magic` of the protected TLV area's info header.
    static constexpr uint16_t PROTECTED_TLV_MAGIC = 0x6908;

private:
    enum class Phase : uint8_t {
        HEADER,
        TLVS,
        DECIDED,
    };

    ImageIdentity own_;
    Phase phase_ = Phase::HEADER;
    ImageIdentityResult result_ = ImageIdentityResult::NEED_MORE;
    // The image is known from its start up to here.
    size_t received_ = 0;
    // The part of the image that window_ holds when complete.
    size_t window_start_ = 0;
    size_t window_size_ = HEADER_SIZE;
    std::array<uint8_t, MAX_PROTECTED_TLV_SIZE> window_{};

    void CopyToWindow(size_t offset, std::span<const uint8_t> chunk);
    [[nodiscard]] bool IsWindowComplete() const;
    ImageIdentityResult Decide(ImageIdentityResult result);
    /** @brief Reads the header; sets up the window over the protected TLVs. */
    ImageIdentityResult ParseHeader(size_t image_size);
    ImageIdentityResult ParseTlvs();

public:
    /** @param own The identity of this unit, which images are compared with. */
    explicit ImageIdentityCollector(const ImageIdentity& own);

    /** @brief Forgets the image seen so far. */
    void Reset();

    /**
     * @brief Takes a chunk of the image.
     * @param offset Where @p chunk starts in the image; 0 starts a new image.
     * @param image_size The declared size of the whole image (`len` of the upload).
     * @return The decision so far: NEED_MORE until the protected TLVs have passed.
     */
    ImageIdentityResult Feed(size_t offset, std::span<const uint8_t> chunk, size_t image_size);

    [[nodiscard]] ImageIdentityResult GetResult() const {
        return result_;
    }
};

} // namespace eerie_leap::subsys::dfu
