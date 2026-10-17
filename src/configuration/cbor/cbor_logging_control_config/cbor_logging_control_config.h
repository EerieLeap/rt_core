#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstddef>
#include <memory_resource>
#include <variant>
#include <vector>
#include <zcbor_common.h>

// A selector as stored: an integer, text (a sensor or setting id) or a bool. The index is the
// choice. Text is a view into the encoded bytes or into the configuration being encoded.
using CborLoggingTriggerSelectorValue = std::variant<int32_t, zcbor_string, bool>;

struct CborLoggingTriggerConfig {
	uint32_t channel{};
	uint32_t event_type{};
	uint32_t payload_key{};
	bool has_selector{};
	uint32_t selector_key{};
	// int 0 when has_selector is false.
	CborLoggingTriggerSelectorValue selector_value{};
	// CDMP UID of the unit to start and stop; 0 is every unit that can log.
	uint32_t target_uid{};
};

struct CborLoggingControlConfig {
	using allocator_type = std::pmr::polymorphic_allocator<>;

	std::pmr::vector<CborLoggingTriggerConfig> CborLoggingTriggerConfig_m;

	CborLoggingControlConfig(std::allocator_arg_t, allocator_type alloc)
		: CborLoggingTriggerConfig_m(alloc) {}

	CborLoggingControlConfig(const CborLoggingControlConfig&) = delete;
	CborLoggingControlConfig& operator=(const CborLoggingControlConfig&) noexcept = default;
	CborLoggingControlConfig& operator=(CborLoggingControlConfig&&) noexcept = default;
	CborLoggingControlConfig(CborLoggingControlConfig&&) noexcept = default;
	~CborLoggingControlConfig() = default;

	CborLoggingControlConfig(CborLoggingControlConfig&& other, allocator_type alloc)
		: CborLoggingTriggerConfig_m(std::move(other.CborLoggingTriggerConfig_m), alloc) {}
};
