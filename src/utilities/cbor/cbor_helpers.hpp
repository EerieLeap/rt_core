#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <zcbor_common.h>

namespace eerie_leap::utilities::cbor {

// zcbor holds text as uint8_t data, std::string and string_view as char: the same bytes viewed through the other type.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
class CborHelpers {
public:
    static std::string ToStdString(const zcbor_string& zstr) {
        return { reinterpret_cast<const char*>(zstr.value), zstr.len };
    }

    static std::string_view ToStringView(const zcbor_string& zstr) {
        if(zstr.value == nullptr)
            return {};

        return { reinterpret_cast<const char*>(zstr.value), zstr.len };
    }

    static std::pmr::string ToPmrString(std::pmr::memory_resource* mr, const zcbor_string& zstr) {
        return { reinterpret_cast<const char*>(zstr.value), zstr.len, mr };
    }

    static zcbor_string ToZcborString(std::string_view str) {
        return { .value = reinterpret_cast<const uint8_t*>(str.data()), .len = static_cast<uint_fast32_t>(str.size()) };
    }
};
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace eerie_leap::utilities::cbor
