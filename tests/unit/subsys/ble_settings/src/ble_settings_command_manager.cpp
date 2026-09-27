#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <vector>

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include "subsys/bluetooth/ble_settings/ble_settings_command/ble_settings_command_manager.h"
#include "subsys/bluetooth/ble_settings/ble_settings_command/ble_settings_command_result.h"

// The commands log to the settings service's module, which this suite doesn't build.
LOG_MODULE_REGISTER(ble_settings_logger);

using namespace eerie_leap::subsys::bluetooth::ble_settings;
using namespace eerie_leap::subsys::bluetooth::ble_settings::ble_settings_command;

namespace {

// Wire values, deliberately not taken from BleSettingsCommandType.
constexpr uint8_t ABORT = 0x01;
constexpr uint8_t START_WRITE = 0x20;
constexpr uint8_t END_WRITE = 0x21;
constexpr uint8_t REQUEST_READ = 0x22;
constexpr uint8_t RESULT = 0x42;

constexpr uint8_t SYSTEM_ID = 0x01;
constexpr uint8_t CANBUS_ID = 0x03;
constexpr size_t MAX_TRANSFER_SIZE = 16;

struct Result {
    uint8_t settings_id;
    BleSettingsState state;
    BleSettingsErrorCode error_code;
};

class CommandHarness {
public:
    std::shared_ptr<BleSettingsStatus> status = std::make_shared<BleSettingsStatus>();
    std::shared_ptr<std::pmr::vector<uint8_t>> buffer =
        std::make_shared<std::pmr::vector<uint8_t>>(MAX_TRANSFER_SIZE);
    BleSettingsCommandManager manager{status};

    std::vector<Result> results;
    std::vector<uint8_t> apply_requests;
    std::vector<uint8_t> read_requests;

    CommandHarness() {
        status->Reset();
        manager.Initialize(buffer, {
            .on_apply_requested = [this](uint8_t settings_id) {
                apply_requests.push_back(settings_id);
            },
            .on_read_requested = [this](uint8_t settings_id) {
                read_requests.push_back(settings_id);
            },
            .on_result = [this](uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code) {
                results.push_back({settings_id, state, error_code});
            },
        });
    }

    void Send(std::initializer_list<uint8_t> command) {
        const std::vector<uint8_t> data(command);
        manager.Process(data);
    }

    // What BleSettingsService does with a Data write during an upload.
    void Receive(std::initializer_list<uint8_t> chunk) {
        std::ranges::copy(chunk, buffer->begin() + status->GetTransferredBytes());
        status->SetTransferredBytes(status->GetTransferredBytes() + chunk.size());
    }
};

void AssertResult(const Result& result, uint8_t settings_id, BleSettingsState state, BleSettingsErrorCode error_code) {
    zassert_equal(result.settings_id, settings_id);
    zassert_equal(result.state, state);
    zassert_equal(result.error_code, error_code);
}

} // namespace

ZTEST_SUITE(ble_settings_commands, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(ble_settings_commands, test_end_write_hands_the_upload_off) {
    CommandHarness harness;

    harness.Send({START_WRITE, CANBUS_ID, 2, 0, 0, 0});
    harness.Receive({0xAA, 0xBB});
    harness.Send({END_WRITE});

    zassert_equal(harness.apply_requests, (std::vector<uint8_t>{CANBUS_ID}));
    zassert_equal(harness.status->GetState(), BleSettingsState::Applying);
    zassert_equal(harness.status->GetSettingsId(), CANBUS_ID);
    zassert_true(harness.results.empty());
}

ZTEST(ble_settings_commands, test_end_write_reports_an_incomplete_upload) {
    CommandHarness harness;

    harness.Send({START_WRITE, CANBUS_ID, 2, 0, 0, 0});
    harness.Receive({0xAA});
    harness.Send({END_WRITE});

    zassert_true(harness.apply_requests.empty());
    zassert_equal(harness.results.size(), 1);
    AssertResult(harness.results[0], CANBUS_ID, BleSettingsState::Error, BleSettingsErrorCode::IncompleteTransfer);
}

ZTEST(ble_settings_commands, test_end_write_without_an_upload_is_rejected) {
    CommandHarness harness;

    harness.Send({END_WRITE});

    zassert_equal(harness.results.size(), 1);
    AssertResult(harness.results[0], 0, BleSettingsState::Error, BleSettingsErrorCode::InvalidState);
}

ZTEST(ble_settings_commands, test_accepted_start_write_reports_nothing) {
    CommandHarness harness;

    harness.Send({START_WRITE, CANBUS_ID, 2, 0, 0, 0});

    zassert_true(harness.results.empty());
    zassert_equal(harness.status->GetState(), BleSettingsState::Writing);
}

ZTEST(ble_settings_commands, test_oversized_start_write_is_rejected) {
    CommandHarness harness;

    harness.Send({START_WRITE, CANBUS_ID, MAX_TRANSFER_SIZE + 1, 0, 0, 0});

    zassert_equal(harness.results.size(), 1);
    AssertResult(harness.results[0], CANBUS_ID, BleSettingsState::Error, BleSettingsErrorCode::TransferTooLarge);
}

ZTEST(ble_settings_commands, test_request_read_hands_the_download_off) {
    CommandHarness harness;

    harness.Send({REQUEST_READ, CANBUS_ID});

    zassert_equal(harness.read_requests, (std::vector<uint8_t>{CANBUS_ID}));
    zassert_equal(harness.status->GetState(), BleSettingsState::Reading);
    zassert_equal(harness.status->GetSettingsId(), CANBUS_ID);
    zassert_true(harness.results.empty());
}

ZTEST(ble_settings_commands, test_request_read_outside_idle_is_rejected) {
    CommandHarness harness;

    harness.Send({START_WRITE, SYSTEM_ID, 2, 0, 0, 0});
    harness.Send({REQUEST_READ, CANBUS_ID});

    zassert_true(harness.read_requests.empty());
    zassert_equal(harness.results.size(), 1);
    AssertResult(harness.results[0], CANBUS_ID, BleSettingsState::Error, BleSettingsErrorCode::InvalidState);
}

ZTEST(ble_settings_commands, test_request_read_while_applying_is_rejected) {
    CommandHarness harness;

    harness.Send({START_WRITE, SYSTEM_ID, 1, 0, 0, 0});
    harness.Receive({0xAA});
    harness.Send({END_WRITE});
    harness.Send({REQUEST_READ, CANBUS_ID});

    zassert_true(harness.read_requests.empty());
    zassert_equal(harness.results.size(), 1);
    AssertResult(harness.results[0], CANBUS_ID, BleSettingsState::Error, BleSettingsErrorCode::InvalidState);
}

ZTEST(ble_settings_commands, test_abort_clears_an_error_without_a_result) {
    CommandHarness harness;

    harness.Send({END_WRITE});
    harness.Send({ABORT});

    zassert_equal(harness.status->GetState(), BleSettingsState::Idle);
    zassert_equal(harness.status->GetErrorCode(), BleSettingsErrorCode::None);
    zassert_equal(harness.results.size(), 1);
}

ZTEST(ble_settings_commands, test_unknown_commands_are_ignored) {
    CommandHarness harness;

    harness.Send({0x7F});

    zassert_true(harness.results.empty());
    zassert_equal(harness.status->GetState(), BleSettingsState::Idle);
}

ZTEST(ble_settings_commands, test_result_notification_layout) {
    auto message = BleSettingsCommandResult::Create(
        CANBUS_ID, BleSettingsState::Error, BleSettingsErrorCode::HandlerFailed);

    zassert_equal(message, (std::vector<uint8_t>{RESULT, CANBUS_ID, 3, 5}));
}
