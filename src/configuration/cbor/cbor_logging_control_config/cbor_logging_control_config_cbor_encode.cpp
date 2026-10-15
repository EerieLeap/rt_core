#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <cstring>
#include <variant>
#include "zcbor_encode.h"
#include "cbor_logging_control_config_cbor_encode.h"
#include "zcbor_print.h"

#define log_result(state, result, func) do { \
	if (!result) { \
		zcbor_trace_file(state); \
		zcbor_log("%s error: %s\r\n", func, zcbor_error_str(zcbor_peek_error(state))); \
	} else { \
		zcbor_log("%s success\r\n", func); \
	} \
} while(0)

static bool encode_CborSelectorValueType(zcbor_state_t *state, const CborLoggingTriggerSelectorValue *input);
static bool encode_CborLoggingTriggerConfig(zcbor_state_t *state, const struct CborLoggingTriggerConfig *input);
static bool encode_CborLoggingControlConfig(zcbor_state_t *state, const struct CborLoggingControlConfig *input);


static bool encode_CborSelectorValueType(
		zcbor_state_t *state, const CborLoggingTriggerSelectorValue *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = false;

	if (const auto *number = std::get_if<int32_t>(input)) {
		res = zcbor_int32_encode(state, number);
	} else if (const auto *text = std::get_if<zcbor_string>(input)) {
		res = zcbor_tstr_encode(state, text);
	} else if (const auto *flag = std::get_if<bool>(input)) {
		res = zcbor_bool_encode(state, flag);
	}

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborLoggingTriggerConfig(
		zcbor_state_t *state, const struct CborLoggingTriggerConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	bool res = (((zcbor_list_start_encode(state, 6) && ((((zcbor_uint32_encode(state, (&(*input).channel))))
	&& ((zcbor_uint32_encode(state, (&(*input).event_type))))
	&& ((zcbor_uint32_encode(state, (&(*input).payload_key))))
	&& ((zcbor_bool_encode(state, (&(*input).has_selector))))
	&& ((zcbor_uint32_encode(state, (&(*input).selector_key))))
	&& (encode_CborSelectorValueType(state, (&(*input).selector_value)))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 6))));

	log_result(state, res, __func__);
	return res;
}

static bool encode_CborLoggingControlConfig(
		zcbor_state_t *state, const struct CborLoggingControlConfig *input)
{
	zcbor_log("%s\r\n", __func__);

	size_t CborLoggingTriggerConfig_m_count = input->CborLoggingTriggerConfig_m.size();

	// The 16 below is the schema's 0*16 (logging_control_config.cddl), as in the decoder.
	bool res = (((zcbor_list_start_encode(state, 1) && ((((zcbor_list_start_encode(state, 16) && ((zcbor_multi_encode_minmax(0, 16, &CborLoggingTriggerConfig_m_count, (zcbor_encoder_t *)encode_CborLoggingTriggerConfig, state, input->CborLoggingTriggerConfig_m.data(), sizeof(struct CborLoggingTriggerConfig))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 16)))) || (zcbor_list_map_end_force_encode(state), false)) && zcbor_list_end_encode(state, 1))));

	log_result(state, res, __func__);
	return res;
}



int cbor_encode_CborLoggingControlConfig(
		uint8_t *payload, size_t payload_len,
		const struct CborLoggingControlConfig *input,
		size_t *payload_len_out)
{
	zcbor_state_t states[5];

	return zcbor_entry_function(payload, payload_len, (void *)input, payload_len_out, states,
		(zcbor_decoder_t *)encode_CborLoggingControlConfig, sizeof(states) / sizeof(zcbor_state_t), 1);
}
