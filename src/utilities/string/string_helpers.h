#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "static_string.hpp"

namespace eerie_leap::utilities::string {

class StringHelpers {
private:
    static std::hash<std::string_view> string_hasher;

public:
    static std::unique_ptr<char[]> ToPaddedCharArray(const std::string& str, size_t size, char padding_char = ' ');
    static size_t GetHash(std::string_view str);

    static StaticString<5> UInt16ToStaticString(uint16_t value);
    static StaticString<10> UInt32ToStaticString(uint32_t value);
};

} // namespace eerie_leap::utilities::string
