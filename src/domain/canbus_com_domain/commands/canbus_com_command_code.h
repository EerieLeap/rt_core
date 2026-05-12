#pragma once

#include <cstdint>

#include "subsys/cdmp/utilities/constants.h"

namespace eerie_leap::domain::canbus_com_domain::commands {

using eerie_leap::subsys::cdmp::utilities::CdmpConstants;

enum class CanbusComCommandCode : uint8_t {
    LOGGING = CdmpConstants::USER_COMMAND_CODE_MIN + 0
};

} // namespace eerie_leap::domain::canbus_com_domain::commands
