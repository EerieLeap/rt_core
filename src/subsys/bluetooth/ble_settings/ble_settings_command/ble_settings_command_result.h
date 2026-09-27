#pragma once

#include <cstdint>
#include <vector>

#include "../ble_settings_service_enums.h"

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

// NOTE: Data format:
//       [0] - BleSettingsCommandType::Result
//       [1] - Settings ID
//       [2] - BleSettingsState
//       [3] - BleSettingsErrorCode
class BleSettingsCommandResult {
public:
    static std::vector<uint8_t> Create(uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code);
};

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
