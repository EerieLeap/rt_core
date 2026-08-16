#include <stdexcept>

#include <zephyr/ztest.h>

#include "subsys/canbus/canbus.h"

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanbusConfig;
using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::CAN_FRAME_MAX_DATA_LENGTH;

ZTEST_SUITE(canbus_config, NULL, NULL, NULL, NULL, NULL);

ZTEST(canbus_config, test_null_device_is_rejected) {
    bool threw = false;

    try {
        Canbus canbus(CanbusConfig(nullptr, CanbusType::CLASSICAL_CAN, 500000));
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw, "Constructing with a null device must throw");
}

ZTEST(canbus_config, test_max_data_length_per_type) {
    zassert_equal(Canbus::GetMaxDataLength(CanbusType::CLASSICAL_CAN), 8);
    zassert_equal(Canbus::GetMaxDataLength(CanbusType::CANFD), CAN_FRAME_MAX_DATA_LENGTH);
}

ZTEST(canbus_config, test_classical_bitrates_are_supported) {
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 500000));
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 250000));
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 10000));
}

ZTEST(canbus_config, test_arbitrary_bitrates_are_rejected) {
    zassert_false(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 123456));
    zassert_false(Canbus::IsBitrateSupported(CanbusType::CANFD, 3000000));
}

ZTEST(canbus_config, test_canfd_only_bitrates) {
    // The high data phase rates are CAN FD only.
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CANFD, 8000000));
    zassert_false(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 8000000));
}

ZTEST(canbus_config, test_zero_bitrate_means_auto_detect) {
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CLASSICAL_CAN, 0));
    zassert_true(Canbus::IsBitrateSupported(CanbusType::CANFD, 0));
}

ZTEST(canbus_config, test_canfd_config_defaults_data_bitrate_to_bitrate) {
    CanbusConfig config(nullptr, CanbusType::CANFD, 500000);

    zassert_equal(config.data_bitrate, 0, "The struct itself must not guess");
    zassert_equal(config.extra_modes, 0);
    zassert_false(config.is_extended_id);
}
