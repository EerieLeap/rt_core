#pragma once

#include <cstdint>
#include <source_location>

#include "utilities/string/string_helpers.h"

namespace eerie_leap::utilities::reflection {

using eerie_leap::utilities::string::StringHelpers;

struct CallerName {
    uint32_t hash = 0;
    const char* name = "";
};

// Pin this in a static constexpr local of the method being identified. The default argument is
// evaluated at the call site, a shared helper would capture its own callers instead, and
// function_name() is specified to be empty anywhere outside a function body.
consteval CallerName GetCallerName(std::source_location location = std::source_location::current()) {
    return { StringHelpers::GetHash(location.function_name()), location.function_name() };
}

} // namespace eerie_leap::utilities::reflection
