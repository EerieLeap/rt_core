#include "ble_settings_command_result.h"

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

std::vector<uint8_t> BleSettingsCommandResult::Create(
    uint8_t settings_id,
    BleSettingsState state,
    BleSettingsErrorCode error_code) {

    return {
        static_cast<uint8_t>(BleSettingsCommandType::Result),
        settings_id,
        static_cast<uint8_t>(state),
        static_cast<uint8_t>(error_code)
    };
}

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
