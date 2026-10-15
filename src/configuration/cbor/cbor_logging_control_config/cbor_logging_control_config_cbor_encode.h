#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "cbor_logging_control_config.h"

int cbor_encode_CborLoggingControlConfig(
		uint8_t *payload, size_t payload_len,
		const struct CborLoggingControlConfig *input,
		size_t *payload_len_out);
