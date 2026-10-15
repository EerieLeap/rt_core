#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "cbor_logging_control_config.h"

int cbor_decode_CborLoggingControlConfig(
		const uint8_t *payload, size_t payload_len,
		struct CborLoggingControlConfig *result,
		size_t *payload_len_out);
