#include "ble_settings_command_request_read.h"

LOG_MODULE_DECLARE(ble_settings_logger);

namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command {

BleSettingsCommandRequestRead::BleSettingsCommandRequestRead(
    std::shared_ptr<BleSettingsStatus> status)
        : BleSettingsCommandRequestBase(status) {}

void BleSettingsCommandRequestRead::Initialize(const ReadRequestedHandler& read_requested_handler) {
    read_requested_handler_ = read_requested_handler;
}

void BleSettingsCommandRequestRead::Process(std::span<const uint8_t> data) {
    if(status_->GetState() != BleSettingsState::Idle) {
        LOG_ERR("RequestRead: not in Idle state");
        status_->SetErrorCode(BleSettingsErrorCode::InvalidState);
        status_->SetState(BleSettingsState::Error);
        return;
    }

    if(data.size() < 2) {
        LOG_ERR("RequestRead: insufficient data");
        status_->SetErrorCode(BleSettingsErrorCode::InsufficientData);
        status_->SetState(BleSettingsState::Error);
        return;
    }

    uint8_t settings_id = data[1];

    if(!read_requested_handler_) {
        LOG_ERR("RequestRead: no read handler registered");
        status_->SetErrorCode(BleSettingsErrorCode::HandlerFailed);
        status_->SetState(BleSettingsState::Error);
        return;
    }

    status_->SetSettingsId(settings_id);
    status_->SetState(BleSettingsState::Reading);
    status_->SetErrorCode(BleSettingsErrorCode::None);

    LOG_INF("RequestRead: settings_id=%u", settings_id);

    read_requested_handler_(settings_id);
}

} // namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command
