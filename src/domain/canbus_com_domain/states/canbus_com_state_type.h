#pragma once

#include <cstdint>

#include "subsys/cdmp/utilities/constants.h"

namespace eerie_leap::domain::canbus_com_domain::states {

using eerie_leap::subsys::cdmp::utilities::CdmpConstants;

/**
 * @brief The state types units share over CDMP, the counterpart of CanbusComCommandCode.
 *
 * Codes and data layouts are part of the network contract: add types and append fields, but never
 * renumber or reuse them.
 */
enum class CanbusComStateType : uint8_t {
    LOGGING_STATUS = CdmpConstants::USER_STATE_TYPE_MIN + 0
};

} // namespace eerie_leap::domain::canbus_com_domain::states
