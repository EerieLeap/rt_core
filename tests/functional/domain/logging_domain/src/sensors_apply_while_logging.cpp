// A sensors configuration stored while logging runs: SensorsController stops the log writer, its
// registered dependent, before the old generation goes, and starts it again on the new one, so the
// first file is finalized and the second describes the new sensors.
#include <memory>
#include <memory_resource>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "controllers/sensors_controller.h"

#include "logging_test_support.h"

using namespace logging_test_support;
using eerie_leap::controllers::SensorsController;

namespace {

std::shared_ptr<Sensor> MakeVirtualSensor(std::string_view id, const char* expression) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = 20;

    return sensor;
}

} // namespace

ZTEST_SUITE(sensors_apply_while_logging, NULL, NULL, ResetAfterTest, NULL, NULL);

ZTEST(sensors_apply_while_logging, test_ASensorsApplyWhileLoggingStartsTheNextFile) {
    auto rig = MakeLogWriterRig();

    // rt_gauge's seven arguments: no configuration service, CAN, GPIO or ADC.
    SensorsController controller(rig.fs_service, rig.config_work_queue, nullptr, rig.time_service, rig.frame, nullptr, nullptr);
    // Stored before the update handler is registered, so it is no restart of its own.
    zassert_equal(controller.Initialize([](auto manager) {
        zassert_true(manager->Update({ MakeVirtualSensor("before_apply", "1 + 1") }), "the first sensors were not stored");
    }), 0, "the sensors controller did not initialize");

    const auto sensors_manager = controller.GetConfigurationManager();

    // As rt_daq's factory: the logger for the sensors stored when the writer starts.
    rig.log_writer_service->SetLoggerFactory([sensors_manager, logging_manager = rig.logging_configuration_manager] {
        return MakeLoggerFactory(logging_manager, *sensors_manager->Get(), true)();
    });
    controller.RegisterDependentService(rig.log_writer_service);

    zassert_equal(controller.Start(), 0, "processing did not start");
    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    k_msleep(300);

    zassert_true(sensors_manager->Update({ MakeVirtualSensor("after_apply", "2 + 2") }), "the new sensors were not stored");
    zassert_true(WaitFor([&] {
        return rig.frame->TryGetReading("after_apply").has_value()
            && rig.log_writer_service->IsRunning()
            && CountLogFiles(*rig.fs_service) == 2;
    }, 3000), "the restart must bring processing and the writer back on the new sensors");
    k_msleep(300);

    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");
    zassert_true(controller.GetProcessingService()->Stop(), "processing did not stop");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 2, "one file per generation (found %zu)", files.size());

    for(size_t i = 0; i < files.size(); i++) {
        AssertFinalizedFile(files[i].content, files[i].path.c_str());

        const auto data_block_address = FindDataBlockAddress(files[i].content);
        zassert_true(files[i].content.size() > data_block_address + kBlockHeaderSize, "file %zu holds no record", i);
    }

    zassert_true(ContainsText(files[0].content, "before_apply"), "the first file must describe the first sensors");
    zassert_false(ContainsText(files[0].content, "after_apply"), "the first file must not describe the new sensors");
    zassert_true(ContainsText(files[1].content, "after_apply"), "the second file must describe the new sensors");
    zassert_false(ContainsText(files[1].content, "before_apply"), "the second file must not describe the first sensors");
}
