// A sensors configuration stored while logging runs: SensorsController stops the log writer, through
// the supervisor's dependent, before the old generation goes, and starts it again on the new one, so
// the first file is finalized and the second describes the new sensors. The supervisor's own factory
// builds the second file's logger at that start, so its header carries the new configuration's CRC
// and the device ID current then. The restart runs on the configuration work queue in turn with the
// requests, so a stop requested during it, or a session that fails during it, is not undone.
//
// The supervisor alone is tested here, without LoggingController's update rule (L9): with it, and
// after LoggingController::Start(), every sensors store stops logging first (logging_control's
// writer_updates), and this restart is what a store during the unit's start does.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "controllers/sensors_controller.h"
#include "subsys/threading/i_service.h"

#include "log_writer_probe.h"
#include "logging_test_support.h"

using namespace logging_test_support;

using eerie_leap::controllers::SensorsController;
using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::ServiceState;

namespace {

std::shared_ptr<Sensor> MakeVirtualSensor(std::string_view id, const char* expression) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = 20;

    return sensor;
}

// Stands in for the supervisor's factory only where a probe wraps it, as the writer cannot hand the
// shipped one back: the logger for the sensors stored when the writer starts, every one of them
// logged. test_ASensorsApplyWhileLoggingStartsTheNextFile covers the supervisor's own factory.
LogWriterService::LoggerFactory MakeStoredSensorsFactory(
    const std::shared_ptr<SensorsConfigurationManager>& sensors_manager,
    const std::shared_ptr<LoggingConfigurationManager>& logging_manager
) {

    return [sensors_manager, logging_manager] {
        return MakeLoggerFactory(logging_manager, *sensors_manager->Get(), true)();
    };
}

// A dependent registered before the writer's, so a restart stops and starts it first. It runs on the
// configuration work queue, where nothing may assert: it records, and the test asserts afterwards.
class FakeDependent final : public IService {
private:
    std::atomic<ServiceState> state_ = ServiceState::RUNNING;

public:
    // Called inside Stop() and Start(), once each, then cleared.
    std::function<void()> on_stop;
    std::function<void()> on_start;
    std::atomic<int> stops = 0;
    std::atomic<int> starts = 0;

    bool Initialize() override {
        return true;
    }

    bool Stop() override {
        if(const auto action = std::exchange(on_stop, nullptr))
            action();

        state_ = ServiceState::STOPPED;
        stops++;

        return true;
    }

    bool Start() override {
        if(const auto action = std::exchange(on_start, nullptr))
            action();

        state_ = ServiceState::RUNNING;
        starts++;

        return true;
    }

    [[nodiscard]] ServiceState GetState() const noexcept override {
        return state_;
    }
};

// Stores @p sensor and waits until processing runs it. The first flush then waits for the restart,
// which may still be starting the dependents, and the second for what the restart queued meanwhile.
void ApplySensors(LogWriterRig& rig, SensorsController& controller, const std::shared_ptr<Sensor>& sensor) {
    const auto id = sensor->id;

    zassert_true(controller.GetConfigurationManager()->Update({ sensor }), "the new sensors were not stored");
    zassert_true(
        WaitFor(
            [&] {
                return rig.frame->TryGetReading(id).has_value();
            },
            3000
        ),
        "the restart must bring processing back on the new sensors"
    );
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

} // namespace

ZTEST_SUITE(sensors_apply_while_logging, NULL, NULL, ResetAfterTest, NULL, NULL);

ZTEST(sensors_apply_while_logging, test_ASensorsApplyWhileLoggingStartsTheNextFile) {
    auto rig = MakeLogWriterRig();
    const auto before = MakeVirtualSensor("before_apply", "1 + 1");
    const auto after = MakeVirtualSensor("after_apply", "2 + 2");

    // rt_gauge's seven arguments: no configuration service, CAN, GPIO or ADC.
    SensorsController controller(
        rig.fs_service, rig.config_work_queue, nullptr, rig.time_service, rig.frame, nullptr, nullptr
    );
    // Stored before the update handler is registered, so it is no restart of its own.
    // NOLINTNEXTLINE(bugprone-lambda-function-name): zassert_*() reports __func__; the lambda is part of the test body
    zassert_equal(
        controller.Initialize([before](const auto& manager) {
            zassert_true(manager->Update({ before }), "the first sensors were not stored");
        }),
        0, "the sensors controller did not initialize"
    );

    const auto sensors_manager = controller.GetConfigurationManager();

    const StatusRecorder recorder;
    const auto device_id = std::make_shared<std::atomic<uint8_t>>(5);
    auto& supervisor = AttachSupervisor(rig, sensors_manager, true, [device_id] {
        return device_id->load();
    });
    controller.RegisterDependentService(supervisor.GetDependentService());
    // Both generations: the logger lists only the sensors stored at its start.
    EnableSensorLogging(rig, std::vector{ before, after });

    zassert_equal(controller.Start(), 0, "processing did not start");
    supervisor.RequestStart(ILoggingControl::ALL_UNITS);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    k_msleep(300);

    const auto before_info = sensors_manager->GetCborConfigurationInfo();
    *device_id = 6;

    zassert_true(sensors_manager->Update({ after }), "the new sensors were not stored");
    zassert_true(
        WaitFor(
            [&] {
                return rig.frame->TryGetReading("after_apply").has_value()
                    && rig.log_writer_service->IsRunning()
                    && CountLogFiles(*rig.fs_service) == 2;
            },
            3000
        ),
        "the restart must bring processing and the writer back on the new sensors"
    );
    k_msleep(300);

    const auto after_info = sensors_manager->GetCborConfigurationInfo();
    zassert_true(before_info.is_applied && after_info.is_applied, "both sensors configurations must be applied");
    zassert_not_equal(before_info.crc, after_info.crc, "the two sensors configurations must differ");

    supervisor.RequestStop(ILoggingControl::ALL_UNITS);
    zassert_true(recorder.WaitForEventCount(4), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped(), "the requested stop must leave the writer stopped");
    zassert_true(controller.GetProcessingService()->Stop(), "processing did not stop");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 2, "one file per generation (found %zu)", files.size());

    for(size_t i = 0; i < files.size(); i++) {
        AssertFinalizedFile(files[i].content, files[i].path.c_str());

        const auto data_block_address = FindDataBlockAddress(files[i].content);
        zassert_true(files[i].content.size() > data_block_address + BLOCK_HEADER_SIZE, "file %zu holds no record", i);
    }

    zassert_true(ContainsText(files[0].content, "before_apply"), "the first file must describe the first sensors");
    zassert_false(ContainsText(files[0].content, "after_apply"), "the first file must not describe the new sensors");
    zassert_true(ContainsText(files[1].content, "after_apply"), "the second file must describe the new sensors");
    zassert_false(
        ContainsText(files[1].content, "before_apply"), "the second file must not describe the first sensors"
    );

    zassert_true(
        ContainsText(files[0].content, Property("sensors_configuration_hash", std::to_string(before_info.crc))),
        "the first file must carry the first sensors configuration's CRC"
    );
    zassert_true(
        ContainsText(files[0].content, Property("device_id", "5")),
        "the first file must carry the device ID of its start"
    );
    zassert_true(
        ContainsText(files[1].content, Property("sensors_configuration_hash", std::to_string(after_info.crc))),
        "the second file must carry the new sensors configuration's CRC"
    );
    zassert_true(
        ContainsText(files[1].content, Property("device_id", "6")),
        "the second file must carry the device ID of its start"
    );
}

// A stop requested while processing restarts reaches the queue behind the restart, so the dependent
// sees it pending and does not bring the writer back; and a writer stopped by a request stays
// stopped across the next restart.
ZTEST(sensors_apply_while_logging, test_AStopRequestedDuringASensorsRestartIsNotUndone) {
    auto rig = MakeLogWriterRig();
    const auto first = MakeVirtualSensor("first", "1 + 1");
    const auto second = MakeVirtualSensor("second", "2 + 2");
    const auto third = MakeVirtualSensor("third", "3 + 3");

    SensorsController controller(
        rig.fs_service, rig.config_work_queue, nullptr, rig.time_service, rig.frame, nullptr, nullptr
    );
    // NOLINTNEXTLINE(bugprone-lambda-function-name): zassert_*() reports __func__; the lambda is part of the test body
    zassert_equal(
        controller.Initialize([first](const auto& manager) {
            zassert_true(manager->Update({ first }), "the first sensors were not stored");
        }),
        0, "the sensors controller did not initialize"
    );

    const auto sensors_manager = controller.GetConfigurationManager();

    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig, sensors_manager, true);
    EnableSensorLogging(rig, std::vector{ first, second, third });

    const auto fake = std::make_shared<FakeDependent>();
    fake->on_start = [&supervisor] {
        supervisor.RequestStop(ILoggingControl::ALL_UNITS);
    };
    controller.RegisterDependentService(fake);
    controller.RegisterDependentService(supervisor.GetDependentService());

    zassert_equal(controller.Start(), 0, "processing did not start");
    supervisor.RequestStart(ILoggingControl::ALL_UNITS);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    ApplySensors(rig, controller, second);
    zassert_equal(fake->starts.load(), 1, "the restart must have started the fake dependent");
    k_msleep(50);

    zassert_true(rig.log_writer_service->IsStopped(), "the stop requested during the restart must stand");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "the writer must not have been started again");
    zassert_false(supervisor.IsActive(), "IsActive() must follow the stop");

    auto events = recorder.GetEvents();
    zassert_equal(events.size(), 2, "one start and the restart's stop (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::Stopped, "the restart stops the writer cleanly");

    // The writer is stopped now, so the next restart leaves it alone.
    ApplySensors(rig, controller, third);
    k_msleep(50);

    zassert_equal(fake->starts.load(), 2, "the second restart must have run");
    zassert_true(rig.log_writer_service->IsStopped(), "a restart must not start a writer that was stopped");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "no file may be opened by the restart");
    events = recorder.GetEvents();
    zassert_equal(
        events.size(), 2, "the restart must publish nothing for a stopped writer (%zu events)", events.size()
    );

    zassert_true(controller.GetProcessingService()->Stop(), "processing did not stop");
}

// A session that fails while processing restarts is stopped by the restart and not started again:
// the next start is a request's.
ZTEST(sensors_apply_while_logging, test_ASessionThatFailsDuringASensorsRestartIsNotRestarted) {
    auto rig = MakeLogWriterRig();

    SensorsController controller(
        rig.fs_service, rig.config_work_queue, nullptr, rig.time_service, rig.frame, nullptr, nullptr
    );
    // NOLINTNEXTLINE(bugprone-lambda-function-name): zassert_*() reports __func__; the lambda is part of the test body
    zassert_equal(
        controller.Initialize([](const auto& manager) {
            zassert_true(manager->Update({ MakeVirtualSensor("first", "1 + 1") }), "the first sensors were not stored");
        }),
        0, "the sensors controller did not initialize"
    );

    const auto sensors_manager = controller.GetConfigurationManager();

    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig, sensors_manager);
    const auto probes = ProbeLoggerFactory::Make(
        MakeStoredSensorsFactory(sensors_manager, rig.logging_configuration_manager)
    );
    rig.log_writer_service->SetLoggerFactory(probes->AsFactory());

    // Fails the session from inside the restart, before the writer's dependent is stopped.
    const auto saw_failure = std::make_shared<std::atomic<bool>>(false);
    const auto fake = std::make_shared<FakeDependent>();
    fake->on_stop = [probes, writer = rig.log_writer_service, saw_failure] {
        probes->Current()->refuse_next_reading = true;
        *saw_failure = WaitFor(
            [&] {
                return writer->HasFailed();
            },
            2000
        );
    };
    controller.RegisterDependentService(fake);
    controller.RegisterDependentService(supervisor.GetDependentService());

    zassert_equal(controller.Start(), 0, "processing did not start");
    supervisor.RequestStart(ILoggingControl::ALL_UNITS);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    k_msleep(150);

    ApplySensors(rig, controller, MakeVirtualSensor("second", "2 + 2"));
    zassert_true(saw_failure->load(), "the session must have failed during the restart");
    zassert_true(recorder.WaitForEventCount(3), "the failure and the restart's stop each publish a status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_true(rig.log_writer_service->IsStopped(), "the restart must leave a failed session stopped");
    zassert_true(rig.log_writer_service->HasFailed(), "nothing may have started the writer since");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "the writer must not have been started again");
    zassert_equal(probes->GetCount(), 1, "only the first start may have built a logger");

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 3, "start, failure, stop (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "the failure must be published");
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the restart's stop publishes Stopped");

    zassert_true(controller.GetProcessingService()->Stop(), "processing did not stop");
}
