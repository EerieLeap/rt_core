#include <stdexcept>

#include <zephyr/ztest.h>

#include "dfu_test_support.h"

using namespace dfu_test;

struct dfu_image_upload_fixture {
    Unit* unit = nullptr;
};

namespace {

void* SetUp() {
    static dfu_image_upload_fixture fixture{};
    fixture.unit = new Unit();

    return &fixture;
}

// Every test starts with empty slots, no upload in progress and the guard and the group registered.
void Before(void* data) {
    auto* fixture = static_cast<dfu_image_upload_fixture*>(data);

    fixture->unit->Start();
    EraseSlot(SLOT0_ID);
    EraseSlot(SLOT1_ID);
    img_mgmt_reset_upload();
}

void TearDown(void* data) {
    delete static_cast<dfu_image_upload_fixture*>(data)->unit;
}

// The number of images img_mgmt lists: one per slot that holds one.
size_t CountListedImages(SmpTestClient& client) {
    const Response response = ReadImages(client);
    zassert_true(response.IsOk());

    return response.list_sizes.at("images");
}

// The chunk that holds the last byte of the protected TLVs, which completes the identity.
size_t IdentityChunkOffset(Image image, size_t chunk_size = CHUNK_SIZE) {
    return (ProtectedAreaEnd(image) - 1) / chunk_size * chunk_size;
}

// Uploads the whole image and expects it to be accepted, checked against its hash and listed.
void ExpectUploadedAndListed(
    SmpTestClient& client,
    Image image,
    std::string_view sha,
    std::string_view version,
    size_t chunk_size = CHUNK_SIZE
) {
    const UploadOutcome outcome = Upload(client, image, sha, chunk_size);

    zassert_false(outcome.is_refused, "Refused at offset %zu", outcome.refused_at);
    zassert_equal(outcome.last.Value("off"), image.size());
    // The unit checked the slot against the hash that came with the first chunk.
    zassert_true(outcome.last.Flag("match"));

    const Response response = ReadImages(client);
    zassert_equal(response.list_sizes.at("images"), 1);
    zassert_equal(response.entries[0].at("slot"), 1);
    zassert_true(response.entry_texts[0].at("version") == version);
    zassert_equal(response.entry_bytes[0].at("hash").size(), SHA_SIZE);
    zassert_true(response.entry_flags[0].at("bootable"));
    zassert_false(response.entry_flags[0].at("active"));
    zassert_false(response.entry_flags[0].at("pending"));
}

// Uploads an image of another identity: the chunk that completes the identity is refused with
// @p error in group 72, the partial image is gone, and the next chunk gets offset 0.
void ExpectRefusedWhenTheIdentityIsComplete(SmpTestClient& client, Image image, std::string_view sha, uint32_t error) {
    const UploadOutcome outcome = Upload(client, image, sha);

    zassert_true(outcome.is_refused);
    zassert_equal(outcome.refused_at, IdentityChunkOffset(image));
    zassert_true(outcome.last.error_group.has_value());
    zassert_equal(*outcome.last.error_group, std::to_underlying(SmpGroupId::DFU));
    zassert_equal(outcome.last.error.value_or(0), error);

    zassert_equal(CountListedImages(client), 0, "The partial image is still listed");
    zassert_true(IsSlot1HeaderErased(), "The header sector is not erased");

    // The unit forgot the upload: a chunk that continues it is answered with offset 0, no error.
    const size_t next = outcome.refused_at + CHUNK_SIZE;
    zassert_true(next < image.size());
    const Response next_response = SendChunk(client, image, sha, next, CHUNK_SIZE, image.size());
    zassert_true(next_response.IsOk());
    zassert_equal(next_response.Value("off"), 0);
}

} // namespace

ZTEST_SUITE(dfu_image_upload, NULL, SetUp, Before, NULL, TearDown);

ZTEST_F(dfu_image_upload, test_upload_of_an_image_for_this_unit_is_accepted_and_listed) {
    ExpectUploadedAndListed(fixture->unit->client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);
}

ZTEST_F(dfu_image_upload, test_chunks_of_other_sizes_are_accepted) {
    for(const size_t chunk_size : { 77U, 300U, 800U }) {
        EraseSlot(SLOT1_ID);
        img_mgmt_reset_upload();

        ExpectUploadedAndListed(
            fixture->unit->client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION, chunk_size
        );
    }
}

ZTEST_F(dfu_image_upload, test_image_already_in_the_slot_completes_with_the_first_chunk) {
    SmpTestClient& client = fixture->unit->client;
    ExpectUploadedAndListed(client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);

    const UploadOutcome outcome = Upload(client, images::GOOD, images::GOOD_SHA256);

    zassert_false(outcome.is_refused);
    zassert_equal(outcome.refused_at, 0);
    zassert_equal(outcome.last.Value("off"), sizeof(images::GOOD));
    zassert_true(outcome.last.Flag("match"));
    zassert_equal(CountListedImages(client), 1);
}

ZTEST_F(dfu_image_upload, test_another_image_replaces_the_one_in_the_slot) {
    SmpTestClient& client = fixture->unit->client;
    ExpectUploadedAndListed(client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);

    ExpectUploadedAndListed(client, images::GOOD_OTHER, images::GOOD_OTHER_SHA256, images::GOOD_OTHER_VERSION);
}

ZTEST_F(dfu_image_upload, test_image_of_another_product_is_refused_when_its_identity_is_complete) {
    ExpectRefusedWhenTheIdentityIsComplete(
        fixture->unit->client, images::WRONG_PRODUCT, images::WRONG_PRODUCT_SHA256, ERROR_WRONG_PRODUCT
    );
}

ZTEST_F(dfu_image_upload, test_image_of_another_board_is_refused_when_its_identity_is_complete) {
    ExpectRefusedWhenTheIdentityIsComplete(
        fixture->unit->client, images::WRONG_BOARD, images::WRONG_BOARD_SHA256, ERROR_WRONG_BOARD
    );
}

ZTEST_F(dfu_image_upload, test_image_with_a_malformed_identity_is_refused_when_its_identity_is_complete) {
    ExpectRefusedWhenTheIdentityIsComplete(
        fixture->unit->client, images::BAD_IDENTITY, images::BAD_IDENTITY_SHA256, ERROR_BAD_IDENTITY
    );
}

ZTEST_F(dfu_image_upload, test_image_without_an_identity_is_refused_with_its_first_chunk) {
    SmpTestClient& client = fixture->unit->client;

    const UploadOutcome outcome = Upload(client, images::NO_IDENTITY, images::NO_IDENTITY_SHA256);

    zassert_true(outcome.is_refused);
    zassert_equal(outcome.refused_at, 0);
    zassert_equal(*outcome.last.error_group, std::to_underlying(SmpGroupId::DFU));
    zassert_equal(outcome.last.error.value_or(0), ERROR_NO_IDENTITY);
    zassert_equal(CountListedImages(client), 0);
}

ZTEST_F(dfu_image_upload, test_image_with_too_many_protected_tlvs_is_refused_with_its_first_chunk) {
    SmpTestClient& client = fixture->unit->client;

    const UploadOutcome outcome = Upload(client, images::OVERSIZE_TLVS, images::OVERSIZE_TLVS_SHA256);

    zassert_true(outcome.is_refused);
    zassert_equal(outcome.refused_at, 0);
    zassert_equal(outcome.last.error.value_or(0), ERROR_BAD_IDENTITY);
    zassert_equal(CountListedImages(client), 0);
}

ZTEST_F(dfu_image_upload, test_refusing_the_first_chunk_leaves_the_image_in_the_slot) {
    SmpTestClient& client = fixture->unit->client;
    ExpectUploadedAndListed(client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);

    const UploadOutcome outcome = Upload(client, images::NO_IDENTITY, images::NO_IDENTITY_SHA256);

    zassert_true(outcome.is_refused);
    zassert_equal(outcome.refused_at, 0);
    // Nothing was written, so the image in the slot, a backup perhaps, is still there.
    const Response response = ReadImages(client);
    zassert_equal(response.list_sizes.at("images"), 1);
    zassert_true(response.entry_texts[0].at("version") == images::GOOD_VERSION);
    zassert_false(IsSlot1HeaderErased());
}

ZTEST_F(dfu_image_upload, test_size_that_cannot_hold_the_protected_tlvs_is_refused_with_the_first_chunk) {
    SmpTestClient& client = fixture->unit->client;

    // Declared 6000 bytes: the protected TLVs of the image end at 6534.
    const Response response = SendChunk(client, images::GOOD, images::GOOD_SHA256, 0, CHUNK_SIZE, 6000);

    zassert_false(response.IsOk());
    zassert_equal(*response.error_group, std::to_underlying(SmpGroupId::DFU));
    zassert_equal(response.error.value_or(0), ERROR_BAD_IDENTITY);
}

ZTEST_F(dfu_image_upload, test_size_above_the_slot_is_refused_by_image_management) {
    SmpTestClient& client = fixture->unit->client;

    const Response response = SendChunk(client, images::GOOD, images::GOOD_SHA256, 0, CHUNK_SIZE, SLOT_SIZE + 1);

    zassert_false(response.IsOk());
    zassert_equal(*response.error_group, MGMT_GROUP_ID_IMAGE);
    zassert_equal(response.error.value_or(0), IMG_MGMT_ERR_INVALID_IMAGE_TOO_LARGE);
    zassert_equal(CountListedImages(client), 0);
}

ZTEST_F(dfu_image_upload, test_resume_at_offset_zero_with_the_same_hash_returns_the_units_offset) {
    SmpTestClient& client = fixture->unit->client;

    // Three chunks, then the connection is lost.
    for(size_t offset = 0; offset < 3 * CHUNK_SIZE; offset += CHUNK_SIZE) {
        const Response response = SendChunk(
            client, images::GOOD, images::GOOD_SHA256, offset, CHUNK_SIZE, sizeof(images::GOOD)
        );
        zassert_true(response.IsOk());
        zassert_equal(response.Value("off"), offset + CHUNK_SIZE);
    }

    // The client starts again at 0 with the same hash and is told where the unit is.
    const Response resumed = SendChunk(client, images::GOOD, images::GOOD_SHA256, 0, CHUNK_SIZE, sizeof(images::GOOD));
    zassert_true(resumed.IsOk());
    zassert_equal(resumed.Value("off"), 3 * CHUNK_SIZE);

    // The check goes on across the resume: the image completes and is accepted.
    const UploadOutcome outcome = Upload(client, images::GOOD, images::GOOD_SHA256, CHUNK_SIZE, 3 * CHUNK_SIZE);
    zassert_false(outcome.is_refused, "Refused at offset %zu", outcome.refused_at);
    zassert_equal(outcome.last.Value("off"), sizeof(images::GOOD));
    zassert_true(outcome.last.Flag("match"));
    zassert_equal(CountListedImages(client), 1);
}

ZTEST_F(dfu_image_upload, test_resume_of_a_refused_image_starts_over) {
    SmpTestClient& client = fixture->unit->client;

    const UploadOutcome refused = Upload(client, images::WRONG_PRODUCT, images::WRONG_PRODUCT_SHA256);
    zassert_true(refused.is_refused);

    // The same hash at offset 0 is a new upload, not a resume into the erased partial image.
    const Response restarted = SendChunk(
        client, images::WRONG_PRODUCT, images::WRONG_PRODUCT_SHA256, 0, CHUNK_SIZE, sizeof(images::WRONG_PRODUCT)
    );
    zassert_true(restarted.IsOk());
    zassert_equal(restarted.Value("off"), CHUNK_SIZE);
}

ZTEST_F(dfu_image_upload, test_new_upload_after_an_abandoned_one_is_checked_on_its_own) {
    SmpTestClient& client = fixture->unit->client;

    // Part of one image of this unit, abandoned.
    for(size_t offset = 0; offset < 4 * CHUNK_SIZE; offset += CHUNK_SIZE) {
        zassert_true(
            SendChunk(client, images::GOOD, images::GOOD_SHA256, offset, CHUNK_SIZE, sizeof(images::GOOD)).IsOk()
        );
    }

    // Another, of another product, is checked from its start.
    const UploadOutcome outcome = Upload(client, images::WRONG_PRODUCT, images::WRONG_PRODUCT_SHA256);
    zassert_true(outcome.is_refused);
    zassert_equal(outcome.refused_at, IdentityChunkOffset(images::WRONG_PRODUCT));
    zassert_equal(outcome.last.error.value_or(0), ERROR_WRONG_PRODUCT);
}

ZTEST_F(dfu_image_upload, test_image_for_this_unit_is_accepted_after_a_refused_one) {
    SmpTestClient& client = fixture->unit->client;

    zassert_true(Upload(client, images::WRONG_BOARD, images::WRONG_BOARD_SHA256).is_refused);

    ExpectUploadedAndListed(client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);
}

ZTEST_F(dfu_image_upload, test_state_write_test_requests_a_swap_on_the_next_boot) {
    SmpTestClient& client = fixture->unit->client;
    ExpectUploadedAndListed(client, images::GOOD, images::GOOD_SHA256, images::GOOD_VERSION);

    zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_NONE);

    // Marks the image test (no `confirm`), by the hash that the list gives.
    const Response listed = ReadImages(client);
    const Response written = client.Write(
        IMG_GROUP, IMG_STATE, CborMap().Put("hash", listed.entry_bytes[0].at("hash")).Build()
    );

    zassert_true(written.IsOk());
    zassert_equal(written.list_sizes.at("images"), 1);
    zassert_true(written.entry_flags[0].at("pending"));
    zassert_false(written.entry_flags[0].at("permanent"));
    zassert_equal(mcuboot_swap_type(), BOOT_SWAP_TYPE_TEST);
}

ZTEST_F(dfu_image_upload, test_without_the_guard_any_image_is_accepted) {
    Unit& unit = *fixture->unit;
    unit.guard.reset();

    const UploadOutcome outcome = Upload(unit.client, images::NO_IDENTITY, images::NO_IDENTITY_SHA256);

    zassert_false(outcome.is_refused);
    zassert_equal(outcome.last.Value("off"), sizeof(images::NO_IDENTITY));
    zassert_equal(CountListedImages(unit.client), 1);
}

ZTEST_F(dfu_image_upload, test_only_one_guard_can_be_registered) {
    Unit& unit = *fixture->unit;
    zassert_true(unit.guard->IsRegistered());

    ImageUploadGuard other(OwnIdentity());
    bool thrown = false;
    try {
        other.Register();
    } catch(const std::logic_error&) {
        thrown = true;
    }

    zassert_true(thrown);
    zassert_false(other.IsRegistered());
    zassert_true(unit.guard->IsRegistered());
}
