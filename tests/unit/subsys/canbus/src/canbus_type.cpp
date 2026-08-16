#include <string>

#include <zephyr/ztest.h>

#include "subsys/canbus/canbus_type.h"

using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::GetCanbusType;
using eerie_leap::subsys::canbus::GetCanbusTypeName;
using eerie_leap::subsys::canbus::IsCanbusTypeValid;

ZTEST_SUITE(canbus_type, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_type, test_names_round_trip) {
    zassert_equal(GetCanbusType("NONE"), CanbusType::NONE);
    zassert_equal(GetCanbusType("CLASSICAL_CAN"), CanbusType::CLASSICAL_CAN);
    zassert_equal(GetCanbusType("CANFD"), CanbusType::CANFD);

    zassert_str_equal(GetCanbusTypeName(CanbusType::NONE), "NONE");
    zassert_str_equal(GetCanbusTypeName(CanbusType::CLASSICAL_CAN), "CLASSICAL_CAN");
    zassert_str_equal(GetCanbusTypeName(CanbusType::CANFD), "CANFD");
}

ZTEST(canbus_type, test_unknown_name_throws) {
    bool threw = false;

    try {
        GetCanbusType("NOT_A_CAN_TYPE");
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "Expected an unknown canbus type name to throw");
}

ZTEST(canbus_type, test_out_of_range_type_is_reported_not_read) {
    auto invalid = static_cast<CanbusType>(200);

    zassert_false(IsCanbusTypeValid(invalid));
    zassert_str_equal(GetCanbusTypeName(invalid), "UNKNOWN");
}

ZTEST(canbus_type, test_accepts_string_and_string_view) {
    std::string name = "CANFD";

    zassert_equal(GetCanbusType(name), CanbusType::CANFD);
    zassert_equal(GetCanbusType(std::string_view(name)), CanbusType::CANFD);
}
