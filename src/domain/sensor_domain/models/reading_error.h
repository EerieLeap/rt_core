#pragma once

#include <cstdint>
#include <string_view>

namespace eerie_leap::domain::sensor_domain::models {

// Why a reading carries ReadingStatus::ERROR. Kept as a code so a reading stays trivially copyable.
enum class ReadingError : uint8_t {
    NONE,
    READER_FAILED,            ///< The source could not produce the reading.
    NO_VALUE,                 ///< A stage needed a value the reading does not have.
    WRONG_STATE,              ///< A stage received a reading in a status it cannot process.
    INPUT_UNAVAILABLE,        ///< An expression input sensor has no value yet.
    EXPRESSION_FAILED,        ///< The expression could not be evaluated.
    EXPRESSION_NOT_A_NUMBER,  ///< The expression evaluated to NaN.
    SCRIPT_FAILED,            ///< The Lua post-processing function failed.
};

constexpr std::string_view ToString(ReadingError error) {
    switch(error) {
    case ReadingError::NONE: return "none";
    case ReadingError::READER_FAILED: return "reader failed";
    case ReadingError::NO_VALUE: return "no value";
    case ReadingError::WRONG_STATE: return "wrong state";
    case ReadingError::INPUT_UNAVAILABLE: return "input unavailable";
    case ReadingError::EXPRESSION_FAILED: return "expression failed";
    case ReadingError::EXPRESSION_NOT_A_NUMBER: return "expression not a number";
    case ReadingError::SCRIPT_FAILED: return "script failed";
    }

    return "unknown";
}

} // namespace eerie_leap::domain::sensor_domain::models
