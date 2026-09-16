#include <zephyr/ztest.h>

#include "subsys/canbus/canbus_type.h"

using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::IsCanbusTypeValid;

ZTEST_SUITE(canbus_type, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_type, test_persisted_type_ids_are_unchanged) {
    zassert_equal(static_cast<uint8_t>(CanbusType::NONE), 0);
    zassert_equal(static_cast<uint8_t>(CanbusType::CLASSICAL_CAN), 1);
    zassert_equal(static_cast<uint8_t>(CanbusType::CANFD), 2);
}

ZTEST(canbus_type, test_defined_types_are_valid) {
    zassert_true(IsCanbusTypeValid(CanbusType::NONE));
    zassert_true(IsCanbusTypeValid(CanbusType::CLASSICAL_CAN));
    zassert_true(IsCanbusTypeValid(CanbusType::CANFD));
}

ZTEST(canbus_type, test_sentinel_and_unknown_types_are_invalid) {
    zassert_false(IsCanbusTypeValid(CanbusType::COUNT));
    zassert_false(IsCanbusTypeValid(static_cast<CanbusType>(200)));
    zassert_false(IsCanbusTypeValid(static_cast<CanbusType>(UINT8_MAX)));
}
