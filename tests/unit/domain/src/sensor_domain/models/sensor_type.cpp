#include <array>
#include <cstdint>
#include <string>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor_configuration.h"
#include "domain/sensor_domain/models/sensor_reading_update_method.h"
#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/models/sensor_type_traits.h"
#include "utilities/memory/memory_resource_manager.h"

using namespace eerie_leap::domain::sensor_domain::models;
using eerie_leap::utilities::memory::Mrm;

ZTEST_SUITE(sensor_type, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr std::array ALL_TYPES = { SensorType::NONE, SensorType::PHYSICAL_ANALOG, SensorType::VIRTUAL_ANALOG,
    SensorType::PHYSICAL_INDICATOR, SensorType::VIRTUAL_INDICATOR, SensorType::CANBUS_RAW, SensorType::CANBUS_ANALOG,
    SensorType::CANBUS_INDICATOR, SensorType::USER_ANALOG, SensorType::USER_INDICATOR, SensorType::CONTROL_INDICATOR,
    SensorType::CONTROL_ANALOG };

} // namespace

ZTEST(sensor_type, test_persisted_type_ids_are_unchanged) {
    for(size_t i = 0; i < ALL_TYPES.size(); ++i)
        zassert_equal(static_cast<uint32_t>(ALL_TYPES[i]), i);
}

// The names are persisted in logs (the sensor_type property): append only, never renamed.
ZTEST(sensor_type, test_names_are_the_enumerators_and_distinct) {
    constexpr std::array<const char*, 12> NAMES = { "NONE", "PHYSICAL_ANALOG", "VIRTUAL_ANALOG", "PHYSICAL_INDICATOR",
        "VIRTUAL_INDICATOR", "CANBUS_RAW", "CANBUS_ANALOG", "CANBUS_INDICATOR", "USER_ANALOG", "USER_INDICATOR",
        "CONTROL_INDICATOR", "CONTROL_ANALOG" };
    static_assert(NAMES.size() == static_cast<size_t>(SensorType::COUNT), "name every type below COUNT");

    for(size_t i = 0; i < NAMES.size(); i++) {
        const std::string name = GetSensorTypeName(static_cast<SensorType>(i));
        zassert_equal(name, NAMES[i], "type %zu is named %s", i, NAMES[i]);

        for(size_t j = 0; j < i; j++)
            zassert_true(name != GetSensorTypeName(static_cast<SensorType>(j)), "type %zu repeats a name", i);
    }

    zassert_equal(std::string(GetSensorTypeName(SensorType::COUNT)), "UNKNOWN");
    zassert_equal(std::string(GetSensorTypeName(static_cast<SensorType>(9999))), "UNKNOWN");
    zassert_equal(std::string(GetSensorTypeName(static_cast<SensorType>(UINT32_MAX))), "UNKNOWN");
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

ZTEST(sensor_type, test_control_types_are_the_ids_after_the_user_types) {
    zassert_equal(static_cast<uint32_t>(SensorType::CONTROL_INDICATOR), 10);
    zassert_equal(static_cast<uint32_t>(SensorType::CONTROL_ANALOG), 11);
    zassert_equal(static_cast<uint32_t>(SensorType::COUNT), 12);
}

ZTEST(sensor_type, test_control_types_take_their_value_from_a_control) {
    for(const auto type : { SensorType::CONTROL_INDICATOR, SensorType::CONTROL_ANALOG }) {
        const auto traits = SensorTypeTraits::Of(type);

        zassert_equal(traits.source, SensorSourceKind::CONTROL);
        zassert_false(traits.requires_channel);
        zassert_false(traits.uses_canbus);
        zassert_false(traits.requires_signal_name);
        zassert_false(traits.requires_interpolation);
        zassert_false(traits.requires_expression);
        zassert_true(traits.allows_expression);
        zassert_false(traits.requires_sampling_rate);
        zassert_false(traits.allows_sampling_rate);
        zassert_true(traits.has_input, "The expression sees the control's value as x");
        zassert_true(traits.is_isr_driven);
    }

    zassert_equal(SensorTypeTraits::Of(SensorType::CONTROL_INDICATOR).value_kind, SensorValueKind::INDICATOR);
    zassert_equal(SensorTypeTraits::Of(SensorType::CONTROL_ANALOG).value_kind, SensorValueKind::ANALOG);
}

ZTEST(sensor_type, test_a_control_sensor_is_updated_by_its_source) {
    SensorConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    for(const auto type : { SensorType::CONTROL_INDICATOR, SensorType::CONTROL_ANALOG }) {
        configuration.type = type;

        zassert_equal(configuration.GetReadingUpdateMethod(), SensorReadingUpdateMethod::ISR);
    }
}
