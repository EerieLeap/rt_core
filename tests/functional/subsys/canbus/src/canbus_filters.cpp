#include <array>
#include <cstdint>

#include <zephyr/ztest.h>

#include "canbus_test_support.h"

using namespace canbus_test;

using eerie_leap::subsys::canbus::CanFilter;
using eerie_leap::subsys::canbus::CanId;

ZTEST_SUITE(canbus_filters, NULL, NULL, ResetLoopbackDevice, NULL, NULL);

namespace {

// Accepts every extended id addressed to `target` in bits 8..15.
constexpr uint32_t TARGET_MASK = 0x1FFFFF00;

CanFilter TargetFilter(uint8_t target) {
    return CanFilter::Masked(CanId::Extended(0x1FF00000 | (static_cast<uint32_t>(target) << 8)), TARGET_MASK);
}

constexpr std::array<uint8_t, 1> PAYLOAD = {0x01};

} // namespace

ZTEST(canbus_filters, test_standard_and_extended_ids_with_same_number_are_distinct) {
    auto canbus = MakeRunningCanbus();
    FrameCollector standard;
    FrameCollector extended;

    zassert_true(canbus->RegisterFrameReceivedHandler(
        CanId::Standard(0x123), [&](const CanFrame& f) { standard.Collect(f); }) > 0);
    zassert_true(canbus->RegisterFrameReceivedHandler(
        CanId::Extended(0x123), [&](const CanFrame& f) { extended.Collect(f); }) > 0);

    zassert_ok(canbus->SendFrame(CanId::Standard(0x123), PAYLOAD));
    zassert_true(standard.Wait(), "Standard frame was not delivered");
    zassert_false(extended.Wait(200), "A standard frame must not reach the extended handler");
    zassert_false(standard.At(0).is_extended);

    zassert_ok(canbus->SendFrame(CanId::Extended(0x123), PAYLOAD));
    zassert_true(extended.Wait(), "Extended frame was not delivered");
    zassert_false(standard.Wait(200), "An extended frame must not reach the standard handler");
    zassert_true(extended.At(0).is_extended);
}

ZTEST(canbus_filters, test_mask_filter_accepts_matching_ids_only) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);

    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00705), PAYLOAD));
    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00742), PAYLOAD));
    zassert_true(collector.Wait());
    zassert_true(collector.Wait());
    zassert_equal(collector.At(0).id, 0x1FF00705);
    zassert_equal(collector.At(1).id, 0x1FF00742);

    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00805), PAYLOAD));
    zassert_false(collector.Wait(200), "Another target must not match");

    zassert_ok(canbus->SendFrame(CanId::Standard(0x705), PAYLOAD));
    zassert_false(collector.Wait(200), "A standard frame must not match an extended mask");

    zassert_true(canbus->RemoveFrameReceivedHandler(id));
}

ZTEST(canbus_filters, test_overlapping_filters_are_rejected) {
    auto canbus = MakeRunningCanbus();

    int mask_id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [](const CanFrame&) {});
    zassert_true(mask_id > 0);

    zassert_equal(
        canbus->RegisterFrameReceivedHandler(CanId::Extended(0x1FF00701), [](const CanFrame&) {}),
        Canbus::ERR_FILTER_OVERLAP,
        "An exact id inside a registered mask must be rejected");
    zassert_equal(
        canbus->RegisterFrameReceivedHandler(
            CanFilter::Masked(CanId::Extended(0x1FF00000), 0x1FF00000), [](const CanFrame&) {}),
        Canbus::ERR_FILTER_OVERLAP,
        "A wider mask covering a registered mask must be rejected");

    int standard_id = canbus->RegisterFrameReceivedHandler(CanId::Standard(0x701), [](const CanFrame&) {});
    zassert_true(standard_id > 0, "Standard ids never overlap extended filters");

    int disjoint_id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x08), [](const CanFrame&) {});
    zassert_true(disjoint_id > 0, "A disjoint mask must be accepted");

    zassert_true(canbus->RemoveFrameReceivedHandler(mask_id));
    zassert_true(canbus->RemoveFrameReceivedHandler(standard_id));
    zassert_true(canbus->RemoveFrameReceivedHandler(disjoint_id));
}

ZTEST(canbus_filters, test_mask_covering_a_registered_id_is_rejected) {
    auto canbus = MakeRunningCanbus();

    int exact_id = canbus->RegisterFrameReceivedHandler(CanId::Extended(0x1FF00705), [](const CanFrame&) {});
    zassert_true(exact_id > 0);

    zassert_equal(
        canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [](const CanFrame&) {}),
        Canbus::ERR_FILTER_OVERLAP);

    zassert_true(canbus->RemoveFrameReceivedHandler(exact_id));
    int mask_id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [](const CanFrame&) {});
    zassert_true(mask_id > 0, "The mask must be accepted once the covered id is gone");

    zassert_true(canbus->RemoveFrameReceivedHandler(mask_id));
}

ZTEST(canbus_filters, test_identical_mask_filters_share_a_registration) {
    auto canbus = MakeRunningCanbus();
    FrameCollector first;
    FrameCollector second;

    // Bits outside the mask are irrelevant, so this is the same filter.
    const CanFilter same_filter = CanFilter::Masked(CanId::Extended(0x1FF007AB), TARGET_MASK);

    int first_id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [&](const CanFrame& f) { first.Collect(f); });
    int second_id = canbus->RegisterFrameReceivedHandler(same_filter, [&](const CanFrame& f) { second.Collect(f); });
    zassert_true(first_id > 0);
    zassert_true(second_id > 0);

    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00701), PAYLOAD));
    zassert_true(first.Wait());
    zassert_true(second.Wait());

    zassert_true(canbus->RemoveFrameReceivedHandler(first_id));

    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00702), PAYLOAD));
    zassert_true(second.Wait(), "The remaining handler must keep receiving");
    zassert_false(first.Wait(200), "A removed handler must not be invoked");

    zassert_true(canbus->RemoveFrameReceivedHandler(second_id));

    FrameCollector third;
    int third_id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [&](const CanFrame& f) { third.Collect(f); });
    zassert_true(third_id > 0, "The filter must be registrable again after the last handler is gone");

    zassert_ok(canbus->SendFrame(CanId::Extended(0x1FF00703), PAYLOAD));
    zassert_true(third.Wait());

    zassert_true(canbus->RemoveFrameReceivedHandler(third_id));
}

ZTEST(canbus_filters, test_mask_filter_limit_is_enforced) {
    auto canbus = MakeRunningCanbus();

    std::array<int, Canbus::MAX_MASK_FILTERS> ids{};
    for(size_t i = 0; i < ids.size(); i++) {
        ids[i] = canbus->RegisterFrameReceivedHandler(TargetFilter(static_cast<uint8_t>(i + 1)), [](const CanFrame&) {});
        zassert_true(ids[i] > 0, "Mask filter %zu should have been accepted", i);
    }

    const CanFilter extra = TargetFilter(static_cast<uint8_t>(Canbus::MAX_MASK_FILTERS + 1));
    zassert_equal(canbus->RegisterFrameReceivedHandler(extra, [](const CanFrame&) {}), Canbus::ERR_TOO_MANY_FILTERS);

    zassert_true(canbus->RemoveFrameReceivedHandler(ids[0]));
    int extra_id = canbus->RegisterFrameReceivedHandler(extra, [](const CanFrame&) {});
    zassert_true(extra_id > 0, "A freed mask slot must be reusable");

    zassert_true(canbus->RemoveFrameReceivedHandler(extra_id));
    for(size_t i = 1; i < ids.size(); i++)
        zassert_true(canbus->RemoveFrameReceivedHandler(ids[i]));
}

ZTEST(canbus_filters, test_invalid_ids_and_filters_are_rejected) {
    auto canbus = MakeRunningCanbus();

    zassert_equal(
        canbus->RegisterFrameReceivedHandler(CanId::Standard(0x800), [](const CanFrame&) {}),
        Canbus::ERR_INVALID_ARGUMENT);
    zassert_equal(
        canbus->RegisterFrameReceivedHandler(CanId::Extended(0x20000000), [](const CanFrame&) {}),
        Canbus::ERR_INVALID_ARGUMENT);
    zassert_equal(
        canbus->RegisterFrameReceivedHandler(CanFilter::Masked(CanId::Standard(0x100), 0xFFF), [](const CanFrame&) {}),
        Canbus::ERR_INVALID_ARGUMENT,
        "A mask wider than the id format must be rejected");

    zassert_equal(canbus->SendFrame(CanId::Standard(0x800), PAYLOAD), -EINVAL);
    zassert_equal(canbus->SendFrame(CanId::Extended(0x20000000), PAYLOAD), -EINVAL);
}

ZTEST(canbus_filters, test_mask_filter_is_deferred_during_auto_detect) {
    // Bitrate 0 selects auto detection, which never completes on the silent loopback bus.
    auto canbus = std::make_unique<Canbus>(MakeConfig(CanbusType::CLASSICAL_CAN, 0));

    zassert_true(canbus->Initialize());
    zassert_true(canbus->Start());
    zassert_false(canbus->IsBitrateDetected());

    int id = canbus->RegisterFrameReceivedHandler(TargetFilter(0x07), [](const CanFrame&) {});
    zassert_true(id > 0, "A deferred mask handler must still return a usable id, got %d", id);
    zassert_true(canbus->RemoveFrameReceivedHandler(id));

    zassert_true(canbus->Stop());
}
