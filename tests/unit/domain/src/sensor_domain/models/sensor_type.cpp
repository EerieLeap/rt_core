#include <array>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor_type.h"

using namespace eerie_leap::domain::sensor_domain::models;

ZTEST_SUITE(sensor_type, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr std::array ALL_TYPES = { SensorType::NONE, SensorType::PHYSICAL_ANALOG, SensorType::VIRTUAL_ANALOG,
    SensorType::PHYSICAL_INDICATOR, SensorType::VIRTUAL_INDICATOR, SensorType::CANBUS_RAW, SensorType::CANBUS_ANALOG,
    SensorType::CANBUS_INDICATOR, SensorType::USER_ANALOG, SensorType::USER_INDICATOR };

} // namespace

ZTEST(sensor_type, test_persisted_type_ids_are_unchanged) {
    for(size_t i = 0; i < ALL_TYPES.size(); ++i)
        zassert_equal(static_cast<uint32_t>(ALL_TYPES[i]), i);
}

ZTEST(sensor_type, test_defined_types_are_valid) {
    for(const auto type : ALL_TYPES)
        zassert_true(IsSensorTypeValid(type));
}

ZTEST(sensor_type, test_sentinel_and_unknown_types_are_invalid) {
    zassert_false(IsSensorTypeValid(SensorType::COUNT));
    zassert_false(IsSensorTypeValid(static_cast<SensorType>(9999)));
    zassert_false(IsSensorTypeValid(static_cast<SensorType>(UINT32_MAX)));
}
