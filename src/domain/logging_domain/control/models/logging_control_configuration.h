#pragma once

#include <cstddef>
#include <memory_resource>
#include <vector>

#include "domain/logging_domain/control/models/logging_control_binding.h"

namespace eerie_leap::domain::logging_domain::control::models {

struct LoggingControlConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    // Each trigger is a subscription on its channel, so the validator caps them. The cap is also the
    // schema's 0*16 (logging_control_config.cddl) and the codec's; the parser asserts they agree.
    static constexpr size_t MAX_TRIGGERS = 16;

    // Default: none.
    std::pmr::vector<LoggingControlBinding> triggers;

    LoggingControlConfiguration(std::allocator_arg_t, allocator_type alloc) :
        triggers(alloc) {}

    LoggingControlConfiguration(const LoggingControlConfiguration&) = delete;
    LoggingControlConfiguration& operator=(const LoggingControlConfiguration&) noexcept = default;
    LoggingControlConfiguration& operator=(LoggingControlConfiguration&&) noexcept = default;
    LoggingControlConfiguration(LoggingControlConfiguration&&) noexcept = default;
    ~LoggingControlConfiguration() = default;

    LoggingControlConfiguration(LoggingControlConfiguration&& other, allocator_type alloc) :
        triggers(std::move(other.triggers), alloc) {}
};

} // namespace eerie_leap::domain::logging_domain::control::models
