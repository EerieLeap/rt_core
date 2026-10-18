#pragma once

#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>

namespace eerie_leap::utilities::concepts {

template<typename T>
concept EnumClassUint32 = std::is_scoped_enum_v<T> && std::same_as<std::underlying_type_t<T>, uint32_t>;

template<typename T>
concept SpanConstructible = std::constructible_from<T, std::span<const uint8_t>>;

} // namespace eerie_leap::utilities::concepts
