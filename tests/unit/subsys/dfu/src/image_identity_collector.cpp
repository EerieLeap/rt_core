#include <utility>

#include <zephyr/ztest.h>

#include "dfu_test_support.h"

using namespace dfu_test;

namespace {

constexpr size_t NO_PAYLOAD = 0;

ImageParts WithArea(Bytes area) {
    ImageParts parts;
    parts.protected_area = std::move(area);

    return parts;
}

// The protected TLV area with the identity of @p identity among filler TLVs, before and after.
Bytes AreaWithFillers(const ImageIdentity& identity) {
    const Bytes filler_value(7, 0x42);

    return ProtectedArea({ Tlv(0x00A1, filler_value), IdentityTlv(identity), Tlv(0x00A2, filler_value) });
}

} // namespace

ZTEST_SUITE(dfu_image_identity_collector, NULL, NULL, NULL, NULL, NULL);

ZTEST(dfu_image_identity_collector, test_image_of_this_unit_matches_in_one_chunk) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeIdentityImage(OwnIdentity());

    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::MATCH);
    zassert_equal(collector.GetResult(), ImageIdentityResult::MATCH);
}

ZTEST(dfu_image_identity_collector, test_identity_among_other_protected_tlvs_is_found) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeImage(WithArea(AreaWithFillers(OwnIdentity())));

    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::MATCH);
}

ZTEST(dfu_image_identity_collector, test_tlv_area_split_at_every_offset_across_two_chunks) {
    const Bytes image = MakeImage(WithArea(AreaWithFillers(OwnIdentity())));

    for(size_t split = 1; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());

        const ImageIdentityResult result = FeedSplit(collector, image, split);
        zassert_equal(
            result, ImageIdentityResult::MATCH, "Split after %zu bytes: result %d", split, static_cast<int>(result)
        );
    }
}

ZTEST(dfu_image_identity_collector, test_a_decision_comes_with_the_chunk_that_completes_the_area) {
    const Bytes image = MakeImage(WithArea(AreaWithFillers(OwnIdentity())));
    const size_t area_end = ImageIdentityCollector::HEADER_SIZE + 300 + AreaWithFillers(OwnIdentity()).size();

    for(size_t split = ImageIdentityCollector::HEADER_SIZE; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());

        const ImageIdentityResult first = collector.Feed(0, std::span(image).first(split), image.size());
        zassert_equal(
            first, split >= area_end ? ImageIdentityResult::MATCH : ImageIdentityResult::NEED_MORE,
            "Split after %zu bytes", split
        );
    }
}

ZTEST(dfu_image_identity_collector, test_one_byte_chunks_match) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeImage(WithArea(AreaWithFillers(OwnIdentity())));

    ImageIdentityResult result = ImageIdentityResult::NEED_MORE;
    for(size_t offset = 0; offset < image.size(); offset++)
        result = collector.Feed(offset, std::span(image).subspan(offset, 1), image.size());

    zassert_equal(result, ImageIdentityResult::MATCH);
}

ZTEST(dfu_image_identity_collector, test_chunks_of_various_sizes_match) {
    const Bytes image = MakeImage(WithArea(AreaWithFillers(OwnIdentity())));

    for(const size_t size : { 7U, 31U, 32U, 33U, 64U, 100U, 256U }) {
        ImageIdentityCollector collector(OwnIdentity());

        ImageIdentityResult result = ImageIdentityResult::NEED_MORE;
        for(size_t offset = 0; offset < image.size(); offset += size) {
            const size_t length = std::min(size, image.size() - offset);
            result = collector.Feed(offset, std::span(image).subspan(offset, length), image.size());
        }

        zassert_equal(result, ImageIdentityResult::MATCH, "Chunks of %zu bytes", size);
    }
}

ZTEST(dfu_image_identity_collector, test_larger_header_size_moves_the_area) {
    ImageParts parts = WithArea(ProtectedArea({ IdentityTlv(OwnIdentity()) }));
    parts.header_size = 0x200;
    const Bytes image = MakeImage(parts);

    for(size_t split = 1; split < image.size(); split += 13) {
        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(FeedSplit(collector, image, split), ImageIdentityResult::MATCH, "Split after %zu bytes", split);
    }
}

ZTEST(dfu_image_identity_collector, test_wrong_product_at_every_split) {
    const Bytes image = MakeIdentityImage(Identity(OWN_PRODUCT + 1, OWN_BOARD));

    for(size_t split = 1; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(
            FeedSplit(collector, image, split), ImageIdentityResult::WRONG_PRODUCT, "Split after %zu bytes", split
        );
    }
}

ZTEST(dfu_image_identity_collector, test_wrong_board_at_every_split) {
    const Bytes image = MakeIdentityImage(Identity(OWN_PRODUCT, "esp32p4_wifi6_touch_lcd_5/esp32p4/hpcore"));

    for(size_t split = 1; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(
            FeedSplit(collector, image, split), ImageIdentityResult::WRONG_BOARD, "Split after %zu bytes", split
        );
    }
}

ZTEST(dfu_image_identity_collector, test_image_without_protected_tlvs_is_refused_with_its_first_chunk) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeImage(ImageParts{});

    // Only the first 64 bytes are sent: the decision does not wait for the end.
    zassert_equal(collector.Feed(0, std::span(image).first(64), image.size()), ImageIdentityResult::NO_IDENTITY);
    // And it stays, whatever follows.
    zassert_equal(collector.Feed(64, std::span(image).subspan(64, 64), image.size()), ImageIdentityResult::NO_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_protected_tlvs_without_identity_are_refused_when_complete) {
    const Bytes image = MakeImage(WithArea(ProtectedArea({ Tlv(0x00A1, Bytes(8, 1)) })));

    for(size_t split = 1; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(
            FeedSplit(collector, image, split), ImageIdentityResult::NO_IDENTITY, "Split after %zu bytes", split
        );
    }
}

ZTEST(dfu_image_identity_collector, test_empty_protected_area_has_no_identity) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeImage(WithArea(ProtectedArea({})));

    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::NO_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_area_of_the_largest_size_is_accepted_and_a_larger_one_refused) {
    // 128 bytes: info header, a filler and the identity.
    const Bytes identity_tlv = IdentityTlv(OwnIdentity());
    const size_t filler_value_size = ImageIdentityCollector::MAX_PROTECTED_TLV_SIZE - 4 - identity_tlv.size() - 4;
    const Bytes largest = ProtectedArea({ Tlv(0x00A1, Bytes(filler_value_size, 9)), identity_tlv });
    zassert_equal(largest.size(), ImageIdentityCollector::MAX_PROTECTED_TLV_SIZE);

    ImageIdentityCollector accepting(OwnIdentity());
    const Bytes accepted = MakeImage(WithArea(largest));
    zassert_equal(accepting.Feed(0, accepted, accepted.size()), ImageIdentityResult::MATCH);

    // One byte more: the area is refused with the first chunk, before it is complete.
    Bytes oversize = ProtectedArea({ Tlv(0x00A1, Bytes(filler_value_size + 1, 9)), identity_tlv });
    zassert_equal(oversize.size(), ImageIdentityCollector::MAX_PROTECTED_TLV_SIZE + 1);

    ImageIdentityCollector refusing(OwnIdentity());
    const Bytes refused = MakeImage(WithArea(oversize));
    zassert_equal(refusing.Feed(0, std::span(refused).first(64), refused.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_header_with_bad_magic_is_refused) {
    ImageParts parts = WithArea(ProtectedArea({ IdentityTlv(OwnIdentity()) }));
    parts.magic = 0x12345678;
    const Bytes image = MakeImage(parts);

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_header_size_below_the_header_is_refused) {
    // The header names a size below its own.
    Bytes image = MakeIdentityImage(OwnIdentity());
    image[8] = ImageIdentityCollector::HEADER_SIZE - 1;

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_area_beyond_the_declared_size_is_refused) {
    const Bytes image = MakeIdentityImage(OwnIdentity());

    // The declared size ends before the protected area does.
    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, std::span(image).first(64), image.size() - 120), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_area_with_a_wrong_info_magic_is_refused) {
    Bytes area = ProtectedArea({ IdentityTlv(OwnIdentity()) });
    area[0] = 0x07;
    area[1] = 0x69;
    const Bytes image = MakeImage(WithArea(area));

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_area_with_a_wrong_total_is_refused) {
    Bytes area = ProtectedArea({ IdentityTlv(OwnIdentity()) });
    area[2]++;
    const Bytes image = MakeImage(WithArea(area));

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_area_too_small_for_its_info_header_is_refused) {
    ImageParts parts;
    parts.protected_area = { 0x08, 0x69, 0x03 };
    const Bytes image = MakeImage(parts);

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_tlv_that_runs_past_the_area_is_refused) {
    Bytes identity_tlv = IdentityTlv(OwnIdentity());
    // Its length field names more than the area holds.
    identity_tlv[2] = static_cast<uint8_t>(identity_tlv[2] + 1);
    const Bytes image = MakeImage(WithArea(ProtectedArea({ identity_tlv })));

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_truncated_tlv_header_at_the_end_of_the_area_is_refused) {
    Bytes area = ProtectedArea({ Tlv(0x00A1, Bytes(4, 1)) });
    // Two stray bytes where another TLV header should start.
    area.push_back(0xA2);
    area.push_back(0x00);
    area[2] = static_cast<uint8_t>(area.size());
    const Bytes image = MakeImage(WithArea(area));

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_malformed_identity_value_is_refused) {
    const Bytes wrong_format = { 2, 0x01, 0xA4, 3, 'a', 'b', 'c' };
    const Bytes wrong_length = { 1, 0x01, 0xA4, 9, 'a', 'b', 'c' };
    const Bytes too_short = { 1, 0x01 };

    for(const Bytes& value : { wrong_format, wrong_length, too_short }) {
        const Bytes image = MakeImage(WithArea(ProtectedArea({ Tlv(ImageIdentity::TLV_TYPE, value) })));

        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
    }
}

ZTEST(dfu_image_identity_collector, test_two_identities_are_refused) {
    const Bytes image = MakeImage(WithArea(ProtectedArea({ IdentityTlv(OwnIdentity()), IdentityTlv(OwnIdentity()) })));

    ImageIdentityCollector collector(OwnIdentity());
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_chunk_that_skips_data_is_refused) {
    const Bytes image = MakeIdentityImage(OwnIdentity());
    ImageIdentityCollector collector(OwnIdentity());

    zassert_equal(collector.Feed(0, std::span(image).first(40), image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(
        collector.Feed(60, std::span(image).subspan(60, 40), image.size()), ImageIdentityResult::BAD_IDENTITY
    );
}

ZTEST(dfu_image_identity_collector, test_data_beyond_the_declared_size_is_refused) {
    const Bytes image = MakeIdentityImage(OwnIdentity());
    ImageIdentityCollector collector(OwnIdentity());

    zassert_equal(collector.Feed(0, std::span(image).first(64), image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(
        collector.Feed(64, std::span(image).subspan(64), image.size() - 1), ImageIdentityResult::BAD_IDENTITY
    );
}

ZTEST(dfu_image_identity_collector, test_a_chunk_sent_again_changes_nothing) {
    const Bytes image = MakeIdentityImage(OwnIdentity());
    ImageIdentityCollector collector(OwnIdentity());

    const size_t second = 100;
    zassert_equal(collector.Feed(0, std::span(image).first(second), image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(
        collector.Feed(second, std::span(image).subspan(second, 50), image.size()), ImageIdentityResult::NEED_MORE
    );
    // The second chunk again (its response got lost), then the rest.
    zassert_equal(
        collector.Feed(second, std::span(image).subspan(second, 50), image.size()), ImageIdentityResult::NEED_MORE
    );
    zassert_equal(
        collector.Feed(second + 50, std::span(image).subspan(second + 50), image.size()), ImageIdentityResult::MATCH
    );
}

ZTEST(dfu_image_identity_collector, test_a_chunk_at_offset_zero_starts_a_new_image) {
    ImageIdentityCollector collector(OwnIdentity());

    const Bytes refused = MakeIdentityImage(Identity(OWN_PRODUCT + 1, OWN_BOARD));
    zassert_equal(collector.Feed(0, refused, refused.size()), ImageIdentityResult::WRONG_PRODUCT);
    zassert_equal(collector.GetResult(), ImageIdentityResult::WRONG_PRODUCT);

    const Bytes accepted = MakeIdentityImage(OwnIdentity());
    zassert_equal(collector.Feed(0, std::span(accepted).first(50), accepted.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(collector.GetResult(), ImageIdentityResult::NEED_MORE);
    zassert_equal(collector.Feed(50, std::span(accepted).subspan(50), accepted.size()), ImageIdentityResult::MATCH);

    // The decision of an image stays, whatever follows it.
    zassert_equal(collector.Feed(50, std::span(accepted).subspan(50), accepted.size()), ImageIdentityResult::MATCH);
}

ZTEST(dfu_image_identity_collector, test_reset_forgets_the_image) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeIdentityImage(OwnIdentity());

    zassert_equal(collector.Feed(0, std::span(image).first(80), image.size()), ImageIdentityResult::NEED_MORE);
    collector.Reset();
    zassert_equal(collector.GetResult(), ImageIdentityResult::NEED_MORE);

    // After a reset the next chunk is the start of an image again.
    zassert_equal(collector.Feed(0, image, image.size()), ImageIdentityResult::MATCH);
    collector.Reset();
    zassert_equal(collector.Feed(80, std::span(image).subspan(80), image.size()), ImageIdentityResult::BAD_IDENTITY);
}

ZTEST(dfu_image_identity_collector, test_empty_chunks_are_harmless) {
    ImageIdentityCollector collector(OwnIdentity());
    const Bytes image = MakeIdentityImage(OwnIdentity());

    zassert_equal(collector.Feed(0, {}, image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(collector.Feed(0, std::span(image).first(40), image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(collector.Feed(40, {}, image.size()), ImageIdentityResult::NEED_MORE);
    zassert_equal(collector.Feed(40, std::span(image).subspan(40), image.size()), ImageIdentityResult::MATCH);
}

ZTEST(dfu_image_identity_collector, test_images_with_no_payload_work) {
    ImageParts parts = WithArea(ProtectedArea({ IdentityTlv(OwnIdentity()) }));
    parts.payload_size = NO_PAYLOAD;
    const Bytes image = MakeImage(parts);

    for(size_t split = 1; split < image.size(); split++) {
        ImageIdentityCollector collector(OwnIdentity());
        zassert_equal(FeedSplit(collector, image, split), ImageIdentityResult::MATCH, "Split after %zu bytes", split);
    }
}
