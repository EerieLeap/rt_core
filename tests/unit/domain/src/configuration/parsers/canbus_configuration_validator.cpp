#include <memory>
#include <stdexcept>

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
