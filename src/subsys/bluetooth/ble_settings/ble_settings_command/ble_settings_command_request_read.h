#pragma once

#include <cstdint>
#include <span>
#include <memory>

#include "ble_settings_command_request_base.h"

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

// NOTE: Data format:
//       [0] - BleSettingsCommandType::RequestRead
//       [1] - Settings ID
class BleSettingsCommandRequestRead : public BleSettingsCommandRequestBase {
public:
    // Starts the download, which runs outside the GATT callback.
    using ReadRequestedHandler = std::function<void(uint8_t settings_id)>;

private:
    ReadRequestedHandler read_requested_handler_;

public:
    explicit BleSettingsCommandRequestRead(std::shared_ptr<BleSettingsStatus> status);
    virtual ~BleSettingsCommandRequestRead() = default;

    void Initialize(const ReadRequestedHandler& read_requested_handler);

    void Process(std::span<const uint8_t> data) override;
};

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
