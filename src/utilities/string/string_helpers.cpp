#include <charconv>

#include "string_helpers.h"

namespace eerie_leap::utilities::string {

std::hash<std::string_view> StringHelpers::string_hasher;

std::unique_ptr<char[]> StringHelpers::ToPaddedCharArray(const std::string& str, size_t size, char padding_char) {
    std::unique_ptr<char[]> char_array = std::make_unique<char[]>(size);
    memset(char_array.get(), padding_char, size);
    std::copy(str.begin(), str.end(), char_array.get());

    return char_array;
}

size_t StringHelpers::GetHash(std::string_view str) {
    return string_hasher(str);
}

template<size_t N, typename T>
StaticString<N> UIntToStaticString(T value) {
    std::array<char, N> buffer;
    auto [ptr, _] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

    return StaticString<N>(std::string_view(buffer.data(), ptr));
}

StaticString<5> StringHelpers::UInt16ToStaticString(uint16_t value) {
    return UIntToStaticString<5>(value);
}

StaticString<10> StringHelpers::UInt32ToStaticString(uint32_t value) {
    return UIntToStaticString<10>(value);
}

} // namespace eerie_leap::utilities::string
