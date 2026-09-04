#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "xx_hash64.hpp"
#include "static_string.hpp"

namespace eerie_leap::utilities::string {

class StringHelpers {
public:
    static std::unique_ptr<char[]> ToPaddedCharArray(const std::string& str, size_t size, char padding_char = ' ');

    static StaticString<5> UInt16ToStaticString(uint16_t value);
    static StaticString<10> UInt32ToStaticString(uint32_t value);

    static constexpr uint32_t GetHash(std::string_view str) {
        return XxHash64::GetHash(str);
    }
};

} // namespace eerie_leap::utilities::string
