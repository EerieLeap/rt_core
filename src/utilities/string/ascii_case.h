#pragma once

#include <algorithm>
#include <string_view>

namespace eerie_leap::utilities::string {

/** @brief @p c with 'A'-'Z' folded to lower case; every other byte as it is (no locale). */
constexpr char ToLowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/** @brief Whether @p a and @p b are the same text but for the case of ASCII letters, as FAT compares names. */
constexpr bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return ToLowerAscii(x) == ToLowerAscii(y);
    });
}

} // namespace eerie_leap::utilities::string
