#pragma once

#include <cstdint>
#include <span>
#include <memory>
#include <functional>

#include "ble_settings_command_request_base.h"

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

// NOTE: Data format:
//       [0] - BleSettingsCommandType::EndWrite
class BleSettingsCommandEndWrite : public BleSettingsCommandRequestBase {
public:
    // Applies the upload, which runs outside the GATT callback.
    using ApplyRequestedHandler = std::function<void(uint8_t settings_id)>;

private:
    ApplyRequestedHandler apply_requested_handler_;

public:
    explicit BleSettingsCommandEndWrite(
        std::shared_ptr<BleSettingsStatus> status);
    virtual ~BleSettingsCommandEndWrite() = default;

    void Initialize(const ApplyRequestedHandler& apply_requested_handler);

    void Process(std::span<const uint8_t> data) override;
};

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
