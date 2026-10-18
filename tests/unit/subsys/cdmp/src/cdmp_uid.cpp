#include <array>
#include <cstdint>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/ztest.h>

#include "subsys/cdmp/utilities/cdmp_uid.h"

#ifndef CONFIG_HWINFO_HAS_DRIVER
    #include "hwinfo_device_id.h"
#endif

using eerie_leap::subsys::cdmp::utilities::CdmpUid;

ZTEST_SUITE(cdmp_uid, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_uid, test_empty_and_blank_ids_are_rejected) {
    constexpr std::array<uint8_t, 6> ZEROS{};
    constexpr std::array<uint8_t, 6> ONES = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    zassert_false(CdmpUid::FromHardwareId({}).has_value());
    zassert_false(CdmpUid::FromHardwareId(ZEROS).has_value(), "An unprogrammed all-zero id has no identity");
    zassert_false(CdmpUid::FromHardwareId(ONES).has_value(), "An unprogrammed all-ones id has no identity");
}

ZTEST(cdmp_uid, test_hardware_id_folds_deterministically) {
    constexpr std::array<uint8_t, 6> MAC_A = { 0x24, 0x6F, 0x28, 0x01, 0x02, 0x03 };
    constexpr std::array<uint8_t, 6> MAC_B = { 0x24, 0x6F, 0x28, 0x01, 0x02, 0x04 };

    auto uid_a = CdmpUid::FromHardwareId(MAC_A);
    auto uid_a_again = CdmpUid::FromHardwareId(MAC_A);
    auto uid_b = CdmpUid::FromHardwareId(MAC_B);

    zassert_true(uid_a.has_value());
    zassert_true(uid_b.has_value());
    zassert_not_equal(uid_a.value(), 0);
    zassert_equal(uid_a.value(), uid_a_again.value(), "The same hardware id must give the same UID");
    zassert_not_equal(uid_a.value(), uid_b.value());
}

// Takes the hardware branch on every board of the suite (a driver, or the fake device id of the QEMU
// boards); the fallback has its own case below.
ZTEST(cdmp_uid, test_generated_uid_is_non_zero) {
    zassert_not_equal(CdmpUid::Generate(), 0);
}

ZTEST(cdmp_uid, test_generated_uid_is_stable_when_hardware_id_exists) {
    std::array<uint8_t, 16> hardware_id{};
    const ssize_t length = hwinfo_get_device_id(hardware_id.data(), hardware_id.size());
    // A board without a hwinfo driver gets a fixed id from fakes/hwinfo_device_id.cpp.
    zassert_true(length > 0, "The board has no hardware id: no hwinfo driver and no fake device id");

    const auto expected = CdmpUid::FromHardwareId({ hardware_id.data(), static_cast<size_t>(length) });
    zassert_true(expected.has_value(), "The hardware id is blank");

    const uint32_t uid = CdmpUid::Generate();
    zassert_equal(uid, expected.value(), "A hardware-derived UID must come from the hardware id");
    zassert_equal(CdmpUid::Generate(), uid, "A hardware-derived UID must not change");
}

#ifndef CONFIG_HWINFO_HAS_DRIVER
// The documented fallback (docs/cdmp.md): without a hardware id each UID is drawn at random. Only
// a board that links the fake device id can have it taken away, which is every board without a
// hwinfo driver.
ZTEST(cdmp_uid, test_generated_uid_falls_back_to_a_random_non_zero_uid_without_a_hardware_id) {
    SetFakeHwinfoDeviceIdAvailable(false);
    const uint32_t first = CdmpUid::Generate();
    const uint32_t second = CdmpUid::Generate();
    SetFakeHwinfoDeviceIdAvailable(true);

    zassert_not_equal(first, 0);
    zassert_not_equal(second, 0);
    zassert_not_equal(first, second, "Without a hardware id each UID is drawn at random");
}
#endif
