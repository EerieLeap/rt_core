#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_validator.h"
#include "domain/canbus_domain/models/can_channel_configuration.h"
#include "domain/canbus_domain/models/can_message_configuration.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::canbus_domain::configuration::parsers;
using eerie_leap::subsys::canbus::CanbusType;

ZTEST_SUITE(canbus_configuration_validator, NULL, NULL, NULL, NULL, NULL);

namespace {

CanbusConfiguration MakeConfiguration(
    CanbusType type,
    bool is_extended_id,
    uint32_t frame_id,
    uint32_t bitrate = 500000) {

    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.type = type;
    channel.bus_channel = 0;
    channel.bitrate = bitrate;
    channel.is_extended_id = is_extended_id;

    auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = frame_id;
    message->name = "EL_FRAME_0";
    message->message_size = 8;
    message->send_interval_ms = 100;
    channel.message_configurations.emplace_back(std::move(message));

    configuration.channel_configurations.emplace(0, std::move(channel));

    return configuration;
}

bool Validates(const CanbusConfiguration& configuration) {
    try {
        CanbusConfigurationValidator::Validate(configuration, nullptr);
    } catch(const std::invalid_argument&) {
        return false;
    }

    return true;
}

CanbusConfiguration MakeConfigurationWithSignalUnit(std::string_view unit) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);

    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.size_bits = 8;
    signal.SetName("sensor_0");
    signal.unit = unit;
    configuration.channel_configurations.at(0).message_configurations.front()->signal_configurations.emplace_back(
        std::move(signal));

    return configuration;
}

} // namespace

ZTEST(canbus_configuration_validator, test_standard_id_on_classical_can_is_valid) {
    zassert_true(Validates(MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123)));
}

ZTEST(canbus_configuration_validator, test_extended_id_on_classical_can_is_valid) {
    // 29-bit identifiers are CAN 2.0B and are supported by classical controllers.
    zassert_true(Validates(MakeConfiguration(CanbusType::CLASSICAL_CAN, true, 0x18FF1234)));
}

ZTEST(canbus_configuration_validator, test_extended_id_on_canfd_is_valid) {
    zassert_true(Validates(MakeConfiguration(CanbusType::CANFD, true, 0x18FF1234)));
}

ZTEST(canbus_configuration_validator, test_frame_id_wider_than_standard_is_rejected) {
    zassert_false(Validates(MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x18FF1234)),
        "A 29-bit frame id must be rejected when the channel is not extended");
}

ZTEST(canbus_configuration_validator, test_frame_id_wider_than_extended_is_rejected) {
    zassert_false(Validates(MakeConfiguration(CanbusType::CLASSICAL_CAN, true, 0x20000000)),
        "A frame id beyond 29 bits must be rejected");
}

ZTEST(canbus_configuration_validator, test_unsupported_bitrate_is_rejected) {
    zassert_false(Validates(MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123, 123456)));
}

ZTEST(canbus_configuration_validator, test_none_sentinel_and_unknown_canbus_types_are_invalid) {
    for(auto type : { CanbusType::NONE, CanbusType::COUNT,
                      static_cast<CanbusType>(200), static_cast<CanbusType>(UINT8_MAX) })
        zassert_false(Validates(MakeConfiguration(type, false, 0x123)),
            "Accepted invalid CAN bus type %u.", static_cast<unsigned>(type));
}

ZTEST(canbus_configuration_validator, test_default_com_configuration_is_valid) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);
    configuration.com_configuration.bus_channel = 0;

    zassert_true(Validates(configuration));
}

ZTEST(canbus_configuration_validator, test_com_channel_must_be_configured) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);
    configuration.com_configuration.bus_channel = 1;

    zassert_false(Validates(configuration), "The COM bus channel must be one of the configured channels");
}

ZTEST(canbus_configuration_validator, test_cdmp_base_must_fit_the_cdmp_range) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);

    configuration.com_configuration.cdmp_base_can_id = 0x79C;
    zassert_true(Validates(configuration));

    configuration.com_configuration.cdmp_base_can_id = 0x79D;
    zassert_false(Validates(configuration));
}

ZTEST(canbus_configuration_validator, test_smp_base_must_be_extended_with_clear_address_bits) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);

    configuration.com_configuration.smp_can_id_base = 0x1FF00001;
    zassert_false(Validates(configuration), "Address bits must be clear");

    configuration.com_configuration.smp_can_id_base = 0x20000000;
    zassert_false(Validates(configuration), "A base beyond 29 bits must be rejected");
}

ZTEST(canbus_configuration_validator, test_smp_bus_share_must_be_a_percentage) {
    auto configuration = MakeConfiguration(CanbusType::CLASSICAL_CAN, false, 0x123);

    for(uint8_t share : {0, 101, UINT8_MAX}) {
        configuration.com_configuration.smp_bus_share_percent = share;
        zassert_false(Validates(configuration), "Accepted bus share %u %%.", static_cast<unsigned>(share));
    }

    for(uint8_t share : {1, 100}) {
        configuration.com_configuration.smp_bus_share_percent = share;
        zassert_true(Validates(configuration), "Rejected bus share %u %%.", static_cast<unsigned>(share));
    }
}

ZTEST(canbus_configuration_validator, test_signal_unit_cannot_be_longer_than_32_characters) {
    zassert_true(Validates(MakeConfigurationWithSignalUnit(std::string(32, 'u'))));
    zassert_false(Validates(MakeConfigurationWithSignalUnit(std::string(33, 'u'))));
}

ZTEST(canbus_configuration_validator, test_signal_unit_may_be_empty) {
    zassert_true(Validates(MakeConfigurationWithSignalUnit("")));
}
