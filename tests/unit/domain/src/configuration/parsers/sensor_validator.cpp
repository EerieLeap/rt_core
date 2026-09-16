#include <memory>
#include <stdexcept>

#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "domain/sensor_domain/configuration/parsers/sensor_validator.h"

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::sensor_domain::models;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorValidator;

ZTEST_SUITE(sensor_validator, NULL, NULL, NULL, NULL, NULL);

namespace {

bool Validates(SensorType type) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "sensor_1");
    sensor->configuration.type = type;

    try {
        SensorValidator::Validate({ sensor }, nullptr, 0, 0);
    } catch(const std::invalid_argument&) {
        return false;
    }

    return true;
}

} // namespace

ZTEST(sensor_validator, test_user_sensor_types_are_valid) {
    zassert_true(Validates(SensorType::USER_ANALOG));
    zassert_true(Validates(SensorType::USER_INDICATOR));
}

ZTEST(sensor_validator, test_none_sentinel_and_unknown_sensor_types_are_invalid) {
    for(auto type : { SensorType::NONE, SensorType::COUNT,
                      static_cast<SensorType>(9999), static_cast<SensorType>(UINT32_MAX) })
        zassert_false(Validates(type), "Accepted invalid sensor type %u.", static_cast<unsigned>(type));
}
