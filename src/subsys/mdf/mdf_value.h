#pragma once

#include <cstdint>
#include <variant>

#include "mdf_data_type.h"

namespace eerie_leap::subsys::mdf {

// Type safe replacement for raw pointers when filling a data record.
using MdfValue = std::variant<int32_t, int64_t, uint32_t, uint64_t, float, double>;

constexpr MdfDataType GetMdfDataType(const MdfValue& value) {
    return std::visit([](auto&& raw) constexpr {
        using T = std::decay_t<decltype(raw)>;

        if constexpr(std::is_same_v<T, int32_t>)
            return MdfDataType::Int32;
        else if constexpr(std::is_same_v<T, int64_t>)
            return MdfDataType::Int64;
        else if constexpr(std::is_same_v<T, uint32_t>)
            return MdfDataType::Uint32;
        else if constexpr(std::is_same_v<T, uint64_t>)
            return MdfDataType::Uint64;
        else if constexpr(std::is_same_v<T, float>)
            return MdfDataType::Float32;
        else
            return MdfDataType::Float64;
    }, value);
}

} // namespace eerie_leap::subsys::mdf
