#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <functional>
#include <unordered_map>

#include "../ble_settings_status.h"
#include "i_ble_settings_command_request.h"
#include "ble_settings_command_start_write.h"
#include "ble_settings_command_end_write.h"
#include "ble_settings_command_request_read.h"
#include "ble_settings_command_abort.h"

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

class BleSettingsCommandManager {
public:
    using ResultHandler = std::function<void(
        uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code)>;

    struct Callbacks {
        BleSettingsCommandEndWrite::ApplyRequestedHandler on_apply_requested;
        BleSettingsCommandRequestRead::ReadRequestedHandler on_read_requested;
        ResultHandler on_result;
    };

private:
    std::shared_ptr<BleSettingsStatus> status_;
    Callbacks callbacks_;
    std::unordered_map<BleSettingsCommandType, std::unique_ptr<IBleSettingsCommandRequest>> commands_;
    std::shared_ptr<std::pmr::vector<uint8_t>> transfer_buffer_;

public:
    explicit BleSettingsCommandManager(std::shared_ptr<BleSettingsStatus> status);
    void Initialize(std::shared_ptr<std::pmr::vector<uint8_t>> transfer_buffer, Callbacks callbacks);

    void Process(std::span<const uint8_t> data);
};

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
