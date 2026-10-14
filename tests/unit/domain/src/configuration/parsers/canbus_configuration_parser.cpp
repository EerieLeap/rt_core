#include <stdexcept>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>
#include <zcbor_encode.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config_cbor_decode.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config_cbor_encode.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_cbor_parser.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::canbus_domain::configuration::parsers;

ZTEST_SUITE(canbus_configuration_parser, NULL, NULL, NULL, NULL, NULL);

CanbusConfiguration canbus_configuration_parser_GetTestConfiguration() {
    CanbusConfiguration canbus_configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    CanChannelConfiguration channel_configuration_0(std::allocator_arg, Mrm::GetDefaultPmr());
    channel_configuration_0.type = CanbusType::CLASSICAL_CAN;
    channel_configuration_0.bus_channel = 0;
    channel_configuration_0.bitrate = 500000;

    auto message_configuration_0_0 = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message_configuration_0_0->frame_id = 0x123;
    message_configuration_0_0->name = "EL_FRAME_0";
    message_configuration_0_0->message_size = 8;
    message_configuration_0_0->send_interval_ms = 100;

    CanSignalConfiguration signal_configuration_0_0_0(std::allocator_arg, Mrm::GetDefaultPmr());
    signal_configuration_0_0_0.start_bit = 0;
    signal_configuration_0_0_0.size_bits = 16;
    signal_configuration_0_0_0.factor = 0.5F;
    signal_configuration_0_0_0.offset = -40.0F;
    signal_configuration_0_0_0.byte_order = CanSignalByteOrder::LITTLE_ENDIAN_INTEL;
    signal_configuration_0_0_0.is_signed = true;
    signal_configuration_0_0_0.SetName("sensor_0");
    signal_configuration_0_0_0.unit = "C";
    message_configuration_0_0->signal_configurations.emplace_back(std::move(signal_configuration_0_0_0));

    CanSignalConfiguration signal_configuration_0_0_1(std::allocator_arg, Mrm::GetDefaultPmr());
    signal_configuration_0_0_1.start_bit = 23;
    signal_configuration_0_0_1.size_bits = 12;
    signal_configuration_0_0_1.factor = 1.0F;
    signal_configuration_0_0_1.offset = 0.0F;
    signal_configuration_0_0_1.byte_order = CanSignalByteOrder::BIG_ENDIAN_MOTOROLA;
    signal_configuration_0_0_1.is_signed = false;
    signal_configuration_0_0_1.SetName("sensor_1");
    signal_configuration_0_0_1.unit = "kmh";
    message_configuration_0_0->signal_configurations.emplace_back(std::move(signal_configuration_0_0_1));

    channel_configuration_0.message_configurations.emplace_back(std::move(message_configuration_0_0));

    auto message_configuration_0_1 = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message_configuration_0_1->frame_id = 0x456;
    message_configuration_0_1->is_enabled = false;
    message_configuration_0_1->name = "EL_FRAME_1";
    message_configuration_0_1->message_size = 8;
    message_configuration_0_1->send_interval_ms = 200;
    channel_configuration_0.message_configurations.emplace_back(std::move(message_configuration_0_1));

    CanChannelConfiguration channel_configuration_1(std::allocator_arg, Mrm::GetDefaultPmr());
    channel_configuration_1.type = CanbusType::CLASSICAL_CAN;
    channel_configuration_1.bus_channel = 1;
    channel_configuration_1.bitrate = 250000;

    auto message_configuration_1_0 = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message_configuration_1_0->frame_id = 0x123;
    message_configuration_1_0->name = "EL_FRAME_0";
    message_configuration_1_0->message_size = 8;
    message_configuration_1_0->send_interval_ms = 100;
    channel_configuration_1.message_configurations.emplace_back(std::move(message_configuration_1_0));

    auto message_configuration_1_1 = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message_configuration_1_1->frame_id = 0x456;
    message_configuration_1_1->name = "EL_FRAME_1";
    message_configuration_1_1->message_size = 8;
    message_configuration_1_1->send_interval_ms = 200;
    channel_configuration_1.message_configurations.emplace_back(std::move(message_configuration_1_1));

    auto message_configuration_1_2 = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message_configuration_1_2->frame_id = 0x789;
    message_configuration_1_2->is_enabled = false;
    message_configuration_1_2->name = "EL_FRAME_2";
    message_configuration_1_2->message_size = 8;
    message_configuration_1_2->send_interval_ms = 300;
    channel_configuration_1.message_configurations.emplace_back(std::move(message_configuration_1_2));

    CanChannelConfiguration channel_configuration_2(std::allocator_arg, Mrm::GetDefaultPmr());
    channel_configuration_2.type = CanbusType::CANFD;
    channel_configuration_2.bus_channel = 2;
    channel_configuration_2.bitrate = 125000;

    canbus_configuration.channel_configurations.emplace(channel_configuration_0.bus_channel, std::move(channel_configuration_0));
    canbus_configuration.channel_configurations.emplace(channel_configuration_1.bus_channel, std::move(channel_configuration_1));
    canbus_configuration.channel_configurations.emplace(channel_configuration_2.bus_channel, std::move(channel_configuration_2));

    canbus_configuration.com_configuration.bus_channel = 1;
    canbus_configuration.com_configuration.cdmp_base_can_id = 0x600;
    canbus_configuration.com_configuration.smp_can_id_base = 0x1E000000;
    canbus_configuration.com_configuration.smp_bus_share_percent = 40;

    return canbus_configuration;
}

void canbus_configuration_parser_CompareCanbusConfigurations(
    CanbusConfiguration& canbus_configuration,
    CanbusConfiguration& deserialized_configuration) {

    zassert_equal(deserialized_configuration.channel_configurations.size(), canbus_configuration.channel_configurations.size());

    const auto& com_configuration = canbus_configuration.com_configuration;
    const auto& deserialized_com_configuration = deserialized_configuration.com_configuration;
    zassert_true(deserialized_com_configuration.bus_channel == com_configuration.bus_channel);
    zassert_equal(deserialized_com_configuration.cdmp_base_can_id, com_configuration.cdmp_base_can_id);
    zassert_equal(deserialized_com_configuration.smp_can_id_base, com_configuration.smp_can_id_base);
    zassert_equal(deserialized_com_configuration.smp_bus_share_percent, com_configuration.smp_bus_share_percent);

    for(auto& [bus_channel, channel_configuration] : canbus_configuration.channel_configurations) {
        zassert_equal(deserialized_configuration.channel_configurations[bus_channel].bus_channel, channel_configuration.bus_channel);
        zassert_equal(deserialized_configuration.channel_configurations[bus_channel].bitrate, channel_configuration.bitrate);

        zassert_equal(deserialized_configuration.channel_configurations[bus_channel].message_configurations.size(), channel_configuration.message_configurations.size());

        for(size_t j = 0; j < channel_configuration.message_configurations.size(); j++) {
            const auto& message_configuration = channel_configuration.message_configurations[j];
            const auto& deserialized_message_configuration = deserialized_configuration.channel_configurations[bus_channel].message_configurations[j];

            zassert_equal(deserialized_message_configuration->frame_id, message_configuration->frame_id);
            zassert_equal(deserialized_message_configuration->is_enabled, message_configuration->is_enabled);
            zassert_equal(deserialized_message_configuration->send_interval_ms, message_configuration->send_interval_ms);
            zassert_equal(deserialized_message_configuration->message_size, message_configuration->message_size);

            zassert_equal(deserialized_message_configuration->signal_configurations.size(), message_configuration->signal_configurations.size());

            for(size_t k = 0; k < message_configuration->signal_configurations.size(); k++) {
                const auto& signal_configuration = message_configuration->signal_configurations[k];
                const auto& deserialized_signal_configuration = deserialized_message_configuration->signal_configurations[k];

                zassert_equal(deserialized_signal_configuration.start_bit, signal_configuration.start_bit);
                zassert_equal(deserialized_signal_configuration.size_bits, signal_configuration.size_bits);
                zassert_equal(deserialized_signal_configuration.factor, signal_configuration.factor);
                zassert_equal(deserialized_signal_configuration.offset, signal_configuration.offset);
                zassert_equal(deserialized_signal_configuration.byte_order, signal_configuration.byte_order);
                zassert_equal(deserialized_signal_configuration.is_signed, signal_configuration.is_signed);
                zassert_equal(deserialized_signal_configuration.name_hash, signal_configuration.name_hash);
                zassert_true(deserialized_signal_configuration.name == signal_configuration.name);
                zassert_true(deserialized_signal_configuration.unit == signal_configuration.unit);
            }
        }
    }
}

ZTEST(canbus_configuration_parser, test_CborSerializeDeserialize) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();

    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);
    auto deserialized_canbus_configuration = canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_canbus_configuration);

    canbus_configuration_parser_CompareCanbusConfigurations(canbus_configuration, *deserialized_canbus_configuration);
}

ZTEST(canbus_configuration_parser, test_CborSerializeDeserialize_without_com_channel) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();
    canbus_configuration.com_configuration = {};

    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);
    auto deserialized_canbus_configuration = canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_canbus_configuration);

    zassert_false(deserialized_canbus_configuration->com_configuration.bus_channel.has_value());
    canbus_configuration_parser_CompareCanbusConfigurations(canbus_configuration, *deserialized_canbus_configuration);
}

ZTEST(canbus_configuration_parser, test_CborDeserialize_rejects_a_com_channel_out_of_range) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();
    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);

    // Channel 0 is configured, which 256 must not wrap to.
    serialized_canbus_configuration->com_config.bus_channel = 256;

    try {
        canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_canbus_configuration);
        zassert_unreachable("A COM bus channel of 256 was accepted");
    } catch(const std::invalid_argument& e) {
        zassert_str_equal(e.what(), "Invalid CAN Bus COM configuration. COM bus channel out of range");
    }
}

ZTEST(canbus_configuration_parser, test_CborDeserialize_rejects_a_channel_out_of_range) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();
    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);

    // 258 would wrap to channel 2, the one it replaces.
    for(auto& channel_config : serialized_canbus_configuration->CborCanChannelConfig_m) {
        if(channel_config.bus_channel == 2)
            channel_config.bus_channel = 258;
    }

    try {
        canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_canbus_configuration);
        zassert_unreachable("A CAN bus channel of 258 was accepted");
    } catch(const std::invalid_argument& e) {
        zassert_str_equal(e.what(), "Invalid CAN Bus configuration. Channel: 258. Bus channel out of range");
    }
}

namespace {

const CborCanChannelConfig* FindCborChannel(const CborCanbusConfig& config, uint32_t bus_channel) {
    for(const auto& channel_config : config.CborCanChannelConfig_m) {
        if(channel_config.bus_channel == bus_channel)
            return &channel_config;
    }

    return nullptr;
}

// One channel with one message, written field by field so that a layout the encoder no longer
// produces can be built: without is_enabled it is the message layout from before the flag.
std::vector<uint8_t> EncodeSingleMessageConfiguration(bool has_is_enabled, bool is_enabled) {
    std::vector<uint8_t> payload(256);
    ZCBOR_STATE_E(state, 8, payload.data(), payload.size(), 1);

    const size_t message_field_count = has_is_enabled ? 7 : 6;

    const bool is_encoded = zcbor_list_start_encode(state, 2)
        && zcbor_list_start_encode(state, 1)
        && zcbor_list_start_encode(state, 6)
        && zcbor_uint32_put(state, std::to_underlying(CanbusType::CLASSICAL_CAN))
        && zcbor_bool_put(state, false)
        && zcbor_uint32_put(state, 0)
        && zcbor_uint32_put(state, 500000)
        && zcbor_uint32_put(state, 0)
        && zcbor_list_start_encode(state, 1)
        && zcbor_list_start_encode(state, message_field_count)
        && zcbor_uint32_put(state, 0x123)
        && (!has_is_enabled || zcbor_bool_put(state, is_enabled))
        && zcbor_int32_put(state, 100)
        && zcbor_tstr_put_lit(state, "")
        && zcbor_tstr_put_lit(state, "EL_FRAME_0")
        && zcbor_uint32_put(state, 8)
        && zcbor_list_start_encode(state, 0)
        && zcbor_list_end_encode(state, 0)
        && zcbor_list_end_encode(state, message_field_count)
        && zcbor_list_end_encode(state, 1)
        && zcbor_list_end_encode(state, 6)
        && zcbor_list_end_encode(state, 1)
        && zcbor_list_start_encode(state, 4)
        && zcbor_int32_put(state, -1)
        && zcbor_uint32_put(state, 0x600)
        && zcbor_uint32_put(state, 0x1E000000)
        && zcbor_uint32_put(state, 40)
        && zcbor_list_end_encode(state, 4)
        && zcbor_list_end_encode(state, 2);

    zassert_true(is_encoded, "Encoding failed: %d", zcbor_peek_error(state));
    payload.resize(static_cast<size_t>(state->payload - payload.data()));

    return payload;
}

} // namespace

ZTEST(canbus_configuration_parser, test_CborSerializeDeserialize_keeps_enabled_and_disabled_messages) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();

    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);

    const auto* channel_config_0 = FindCborChannel(*serialized_canbus_configuration, 0);
    zassert_not_null(channel_config_0);
    zassert_true(channel_config_0->CborCanMessageConfig_m.at(0).is_enabled);
    zassert_false(channel_config_0->CborCanMessageConfig_m.at(1).is_enabled);

    auto deserialized_canbus_configuration = canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_canbus_configuration);

    const auto& messages_0 = deserialized_canbus_configuration->channel_configurations.at(0).message_configurations;
    zassert_true(messages_0.at(0)->is_enabled);
    zassert_false(messages_0.at(1)->is_enabled);
    // A disabled message keeps what it would send with.
    zassert_equal(messages_0.at(1)->send_interval_ms, 200);

    const auto& messages_1 = deserialized_canbus_configuration->channel_configurations.at(1).message_configurations;
    zassert_true(messages_1.at(0)->is_enabled);
    zassert_true(messages_1.at(1)->is_enabled);
    zassert_false(messages_1.at(2)->is_enabled);
    zassert_equal(messages_1.at(2)->send_interval_ms, 300);
}

ZTEST(canbus_configuration_parser, test_CborEncodeDecode_keeps_enabled_and_disabled_messages) {
    CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);

    auto canbus_configuration = canbus_configuration_parser_GetTestConfiguration();
    auto serialized_canbus_configuration = canbus_configuration_cbor_parser.Serialize(canbus_configuration);

    std::vector<uint8_t> payload(2048);
    size_t encoded_size = 0;
    zassert_ok(cbor_encode_CborCanbusConfig(payload.data(), payload.size(), serialized_canbus_configuration.get(), &encoded_size));

    CborCanbusConfig decoded_config(std::allocator_arg, Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_ok(cbor_decode_CborCanbusConfig(payload.data(), encoded_size, &decoded_config, &decoded_size));
    zassert_equal(decoded_size, encoded_size);

    auto deserialized_canbus_configuration = canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), decoded_config);

    canbus_configuration_parser_CompareCanbusConfigurations(canbus_configuration, *deserialized_canbus_configuration);
}

ZTEST(canbus_configuration_parser, test_CborDecode_reads_is_enabled_after_the_frame_id) {
    for(const bool is_enabled : {true, false}) {
        const auto payload = EncodeSingleMessageConfiguration(true, is_enabled);

        CborCanbusConfig decoded_config(std::allocator_arg, Mrm::GetDefaultPmr());
        size_t decoded_size = 0;
        zassert_ok(cbor_decode_CborCanbusConfig(payload.data(), payload.size(), &decoded_config, &decoded_size));
        zassert_equal(decoded_size, payload.size());

        const auto& message_config = decoded_config.CborCanChannelConfig_m.at(0).CborCanMessageConfig_m.at(0);
        zassert_equal(message_config.frame_id, 0x123);
        zassert_equal(message_config.is_enabled, is_enabled);
        zassert_equal(message_config.send_interval_ms, 100);

        CanbusConfigurationCborParser canbus_configuration_cbor_parser(nullptr);
        auto configuration = canbus_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), decoded_config);
        zassert_equal(configuration->channel_configurations.at(0).message_configurations.at(0)->is_enabled, is_enabled);
    }
}

ZTEST(canbus_configuration_parser, test_CborDecode_rejects_a_message_without_is_enabled) {
    // The layout stored before the flag existed; there is no migration from it.
    const auto payload = EncodeSingleMessageConfiguration(false, true);

    CborCanbusConfig decoded_config(std::allocator_arg, Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_not_equal(cbor_decode_CborCanbusConfig(payload.data(), payload.size(), &decoded_config, &decoded_size), 0,
        "A message without is_enabled was decoded");
}
