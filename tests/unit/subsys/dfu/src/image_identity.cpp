#include <array>
#include <string>

#include <zephyr/ztest.h>

#include "dfu_test_support.h"

using namespace dfu_test;

ZTEST_SUITE(dfu_image_identity, NULL, NULL, NULL, NULL, NULL);

ZTEST(dfu_image_identity, test_encode_is_format_product_length_board) {
    const ImageIdentity identity = Identity(0xA401, "native_sim/native");

    std::array<uint8_t, ImageIdentity::MAX_ENCODED_SIZE> buffer{};
    const size_t size = identity.Encode(buffer);

    const std::string_view board = "native_sim/native";
    zassert_equal(size, 4 + board.size());
    zassert_equal(buffer[0], 1, "format");
    zassert_equal(buffer[1], 0x01, "product, low byte first");
    zassert_equal(buffer[2], 0xA4);
    zassert_equal(buffer[3], board.size());
    zassert_true(std::equal(board.begin(), board.end(), buffer.begin() + 4));
}

ZTEST(dfu_image_identity, test_encode_into_a_buffer_that_is_too_small_writes_nothing) {
    const ImageIdentity identity = OwnIdentity();

    std::array<uint8_t, 4 + OWN_BOARD.size() - 1> buffer{};
    zassert_equal(identity.Encode(buffer), 0);
    zassert_true(std::ranges::all_of(buffer, [](uint8_t byte) {
        return byte == 0;
    }));
}

ZTEST(dfu_image_identity, test_parse_returns_what_was_encoded) {
    const ImageIdentity identity = OwnIdentity();

    std::array<uint8_t, ImageIdentity::MAX_ENCODED_SIZE> buffer{};
    const size_t size = identity.Encode(buffer);

    const auto parsed = ImageIdentity::Parse(std::span(buffer).first(size));

    zassert_true(parsed.has_value());
    zassert_true(*parsed == identity);
    zassert_equal(parsed->GetProductId(), OWN_PRODUCT);
    zassert_true(parsed->GetBoard() == OWN_BOARD);
}

ZTEST(dfu_image_identity, test_longest_board_fits_and_longer_does_not) {
    const std::string longest(ImageIdentity::MAX_BOARD_LENGTH, 'b');

    const auto fits = ImageIdentity::Create(1, longest);
    zassert_true(fits.has_value());
    zassert_equal(fits->GetBoard().size(), ImageIdentity::MAX_BOARD_LENGTH);

    zassert_false(ImageIdentity::Create(1, longest + "b").has_value());
}

ZTEST(dfu_image_identity, test_longest_board_fills_the_whole_protected_area) {
    const std::string longest(ImageIdentity::MAX_BOARD_LENGTH, 'b');
    const ImageIdentity identity = Identity(OWN_PRODUCT, longest);
    const Bytes image = MakeIdentityImage(identity);

    ImageIdentityCollector collector(identity);
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::MATCH);

    // The area is the 128 bytes the unit accepts.
    zassert_equal(ProtectedArea({ IdentityTlv(identity) }).size(), ImageIdentityCollector::MAX_PROTECTED_TLV_SIZE);
}

ZTEST(dfu_image_identity, test_board_must_be_printable_ascii) {
    zassert_false(ImageIdentity::Create(1, std::string_view("a\tb")).has_value());
    zassert_false(ImageIdentity::Create(1, std::string_view("a\0b", 3)).has_value());
    zassert_false(ImageIdentity::Create(1, "caf\xC3\xA9").has_value());
    zassert_true(ImageIdentity::Create(1, "").has_value(), "An empty board is well-formed; it never matches");
}

ZTEST(dfu_image_identity, test_parse_refuses_malformed_values) {
    const Bytes good = { 1, 0x01, 0xA4, 3, 'a', 'b', 'c' };
    zassert_true(ImageIdentity::Parse(good).has_value());

    // Too short for the fixed part.
    zassert_false(ImageIdentity::Parse(std::span(good).first(3)).has_value());
    zassert_false(ImageIdentity::Parse({}).has_value());

    // Another format.
    Bytes other_format = good;
    other_format[0] = 2;
    zassert_false(ImageIdentity::Parse(other_format).has_value());

    // The board length says more, or less, than the value holds.
    Bytes longer = good;
    longer[3] = 4;
    zassert_false(ImageIdentity::Parse(longer).has_value());
    Bytes shorter = good;
    shorter[3] = 2;
    zassert_false(ImageIdentity::Parse(shorter).has_value());

    // A character outside printable ASCII.
    Bytes control = good;
    control[5] = 0x07;
    zassert_false(ImageIdentity::Parse(control).has_value());
    Bytes high = good;
    high[5] = 0xC3;
    zassert_false(ImageIdentity::Parse(high).has_value());

    // A board above the limit.
    Bytes oversize = { 1, 0x01, 0xA4, ImageIdentity::MAX_BOARD_LENGTH + 1 };
    oversize.insert(oversize.end(), ImageIdentity::MAX_BOARD_LENGTH + 1, 'x');
    zassert_false(ImageIdentity::Parse(oversize).has_value());
}

ZTEST(dfu_image_identity, test_check_compares_product_then_board) {
    const ImageIdentity own = OwnIdentity();

    zassert_equal(own.Check(Identity(OWN_PRODUCT, OWN_BOARD)), ImageIdentityResult::MATCH);
    zassert_equal(own.Check(Identity(OWN_PRODUCT + 1, OWN_BOARD)), ImageIdentityResult::WRONG_PRODUCT);
    zassert_equal(
        own.Check(Identity(OWN_PRODUCT, "esp32p4_wifi6_touch_lcd_5/esp32p4/hpcore")), ImageIdentityResult::WRONG_BOARD
    );
    // A prefix of the board is another board.
    zassert_equal(
        own.Check(Identity(OWN_PRODUCT, "esp32s3_touch_amoled_1_75/esp32s3")), ImageIdentityResult::WRONG_BOARD
    );
    // Both differ: the product is what is reported.
    zassert_equal(own.Check(Identity(0x1234, "native_sim")), ImageIdentityResult::WRONG_PRODUCT);
}
