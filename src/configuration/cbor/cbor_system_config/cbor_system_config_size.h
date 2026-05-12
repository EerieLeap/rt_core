#pragma once

#include "utilities/cbor/cbor_size_builder.hpp"

#include "cbor_system_config.h"

using eerie_leap::utilities::cbor::CborSizeBuilder;

static size_t cbor_get_size_CborSystemConfig(const CborSystemConfig& config) {
    CborSizeBuilder builder;
    builder.AddIndefiniteArrayStart();

    builder.AddUint(config.device_id)
        .AddUint(config.build_number)
        .AddUint(config.json_config_checksum);

    return builder.Build();
}
