#include <array>
#include <cstdint>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/ztest.h>

#include "subsys/cdmp/utilities/cdmp_uid.h"

using eerie_leap::subsys::cdmp::utilities::CdmpUid;

ZTEST_SUITE(cdmp_uid, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_uid, test_empty_and_blank_ids_are_rejected) {
    constexpr std::array<uint8_t, 6> zeros{};
    constexpr std::array<uint8_t, 6> ones = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    zassert_false(CdmpUid::FromHardwareId({}).has_value());
    zassert_false(CdmpUid::FromHardwareId(zeros).has_value(), "An unprogrammed all-zero id has no identity");
    zassert_false(CdmpUid::FromHardwareId(ones).has_value(), "An unprogrammed all-ones id has no identity");
}

ZTEST(cdmp_uid, test_hardware_id_folds_deterministically) {
    constexpr std::array<uint8_t, 6> mac_a = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x03};
    constexpr std::array<uint8_t, 6> mac_b = {0x24, 0x6F, 0x28, 0x01, 0x02, 0x04};

    auto uid_a = CdmpUid::FromHardwareId(mac_a);
    auto uid_a_again = CdmpUid::FromHardwareId(mac_a);
    auto uid_b = CdmpUid::FromHardwareId(mac_b);

    zassert_true(uid_a.has_value());
    zassert_true(uid_b.has_value());
    zassert_not_equal(uid_a.value(), 0);
    zassert_equal(uid_a.value(), uid_a_again.value(), "The same hardware id must give the same UID");
    zassert_not_equal(uid_a.value(), uid_b.value());
}

ZTEST(cdmp_uid, test_generated_uid_is_non_zero) {
    zassert_not_equal(CdmpUid::Generate(), 0);
}

ZTEST(cdmp_uid, test_generated_uid_is_stable_when_hardware_id_exists) {
    std::array<uint8_t, 16> hardware_id{};
    const ssize_t length = hwinfo_get_device_id(hardware_id.data(), hardware_id.size());
    if(length <= 0)
        ztest_test_skip();

    zassert_equal(CdmpUid::Generate(), CdmpUid::Generate(), "A hardware-derived UID must not change");
}
