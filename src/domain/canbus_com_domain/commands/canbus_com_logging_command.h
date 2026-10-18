#pragma once

#include <cstdint>

#include "i_canbus_com_command.h"

namespace eerie_leap::domain::canbus_com_domain::commands {

class CanbusComLoggingCommand : public ICanbusComCommand {
private:
    bool is_start_ = false;

public:
    CanbusComLoggingCommand(bool is_start) :
        is_start_(is_start) {}
    CanbusComLoggingCommand(std::span<const uint8_t> data) {
        is_start_ = data[0] == 0x01;
    }

    virtual ~CanbusComLoggingCommand() = default;

    [[nodiscard]] bool IsStart() const {
        return is_start_;
    }

    [[nodiscard]] CanbusComCommandCode GetCommandCode() const override {
        return CanbusComCommandCode::LOGGING;
    }

    [[nodiscard]] std::vector<uint8_t> GetData() const override {
        return { static_cast<uint8_t>(is_start_ ? 0x01 : 0x00) };
    }
};

} // namespace eerie_leap::domain::canbus_com_domain::commands
