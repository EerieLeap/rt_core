#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include "zcbor_decode.h"
#include "cbor_logging_control_config_cbor_decode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
	if (!result) { \
		zcbor_trace_file(state); \
		zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
	} else { \
		zcbor_log("%s success\r\n", func); \
	} \
} while(0)

// CborLoggingControlConfig = [triggers: [0*16 CborLoggingTriggerConfig]]
// The cap is the schema's (logging_control_config.cddl), the encoder's and
// LoggingControlConfiguration::kMaxTriggers; the CBOR parser asserts the last agrees.
static constexpr size_t k_max_triggers = 16;

static bool decode_CborSelectorValueType(zcbor_state_t *state, CborLoggingTriggerSelectorValue *result);
static bool decode_CborLoggingTriggerConfig(zcbor_state_t *state, struct CborLoggingTriggerConfig *result);
static bool decode_CborLoggingControlConfig(zcbor_state_t *state, struct CborLoggingControlConfig *result);


// int / tstr / bool: each alternative is tried in turn from the same position, as the gauge's
// decode_CborPropertyValueType does.
static bool decode_CborSelectorValueType(
		zcbor_state_t *state, CborLoggingTriggerSelectorValue *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = false;
	zcbor_state_t backup_state = *state;

	{
		int32_t val;
		if (zcbor_int32_decode(state, &val)) {
			*result = val;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		zcbor_string val;
		if (zcbor_tstr_decode(state, &val)) {
			*result = val;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
		bool val;
		if (zcbor_bool_decode(state, &val)) {
			*result = val;
			res = true;
		}
	}

	if (!res) {
		*state = backup_state;
	}

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborLoggingTriggerConfig(
		zcbor_state_t *state, struct CborLoggingTriggerConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_decode(state) && ((((zcbor_uint32_decode(state, (&(*result).channel))))
	&& ((zcbor_uint32_decode(state, (&(*result).event_type))))
	&& ((zcbor_uint32_decode(state, (&(*result).payload_key))))
	&& ((zcbor_bool_decode(state, (&(*result).has_selector))))
	&& ((zcbor_uint32_decode(state, (&(*result).selector_key))))
	&& (decode_CborSelectorValueType(state, (&(*result).selector_value)))
	&& ((zcbor_uint32_decode(state, (&(*result).target_uid))))) || (zcbor_list_map_end_force_decode(state), false)) && zcbor_list_end_decode(state))));

	log_result(state, res, __func__);
	return res;
}

static bool decode_CborLoggingControlConfig(
		zcbor_state_t *state, struct CborLoggingControlConfig *result)
{
	zcbor_log("%s\r\n", __func__);

	if (!zcbor_list_start_decode(state)) {
		return false;
	}

	if (!zcbor_list_start_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	while (!zcbor_array_at_end(state)) {
		// The schema's 0*16, checked before the element is stored.
		if (result->CborLoggingTriggerConfig_m.size() >= k_max_triggers) {
			zcbor_list_map_end_force_decode(state);
			zcbor_list_end_decode(state);
			zcbor_list_end_decode(state);
			return false;
		}

		result->CborLoggingTriggerConfig_m.emplace_back();
		if (!decode_CborLoggingTriggerConfig(state, &result->CborLoggingTriggerConfig_m.back())) {
			result->CborLoggingTriggerConfig_m.pop_back();
			zcbor_list_map_end_force_decode(state);
			zcbor_list_end_decode(state);
			zcbor_list_end_decode(state);
			return false;
		}
	}

	if (!zcbor_list_end_decode(state)) {
		zcbor_list_end_decode(state);
		return false;
	}

	if (!zcbor_list_end_decode(state)) {
		return false;
	}

	log_result(state, true, __func__);
	return true;
}



int cbor_decode_CborLoggingControlConfig(
		const uint8_t *payload, size_t payload_len,
		struct CborLoggingControlConfig *result,
		size_t *payload_len_out)
{
	zcbor_state_t states[5];

	return zcbor_entry_function(payload, payload_len, (void *)result, payload_len_out, states,
		(zcbor_decoder_t *)decode_CborLoggingControlConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
