// The log writer's session: status events, a volume that fills up, rotation at the size limit,
// the free-space minimum, a restart for a new logging configuration, dropped raw frames and a stop
// in the middle of a tick. Starts and stops go through the LogWriterSupervisor where it decides
// something; its own policy is in log_writer_supervisor.cpp.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor_limits.h"

#include "memory_stream_buf.h"

#include "log_writer_probe.h"
#include "logging_test_support.h"

using namespace logging_test_support;

using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::sensor_domain::models::SensorLimits;
using rt_core_test::MemoryStreamBuf;

namespace {

// The bytes StartLogging() writes for the rig's sensors: what a file holds before its first record.
uint64_t GetHeaderSize(LogWriterRig& rig) {
    MemoryStreamBuf stream;
    auto logger = rig.logger_factory();
    zassert_true(logger->StartLogging(stream, rig.time_service->GetCurrentTime()), "the header could not be written to memory");

    return logger->GetBytesWritten();
}

// FAT hands out a fixed number of file objects; one the writer still held would be missing here.
bool CanOpenEveryFileObject(FsService& fs_service, const std::string& path) {
    std::vector<fs_file_t> files(CONFIG_FS_FATFS_NUM_FILES);
    size_t opened = 0;

    for(auto& file : files) {
        fs_file_t_init(&file);
        if(fs_service.OpenFile(path, FS_O_READ, &file) != 0)
            break;

        opened++;
    }

    for(size_t i = 0; i < opened; i++)
        fs_close(&files[i]);

    return opened == files.size();
}

// Raw frames at a steady rate, 20 per 100 ms tick, until @p done holds, so the file keeps growing.
template<typename TCondition>
bool PublishFramesUntil(LogWriterRig& rig, const Sensor& sensor, TCondition done, int64_t timeout_ms) {
    uint8_t sequence = 0;

    return WaitFor([&] {
        for(int i = 0; i < 2; i++)
            PublishFrame(rig, sensor, sequence++, rig.time_service->GetCurrentTime());

        return done();
    }, timeout_ms);
}

} // namespace

ZTEST_SUITE(log_writer_lifecycle, NULL, NULL, ResetAfterTest, NULL, NULL);

ZTEST(log_writer_lifecycle, test_StartAndStopPublishTheirStatus) {
    auto rig = MakeLogWriterRig();
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "Start must publish StatusUpdated");
    zassert_true(WaitFor([&] { return supervisor.IsActive(); }, 1000), "IsActive() must follow the event");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "Stop must publish StatusUpdated");
    zassert_true(WaitFor([&] { return !supervisor.IsActive(); }, 1000), "IsActive() must follow the event");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(100);

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 2, "one event per transition, and no stop after a clean one (got %zu)", events.size());
    zassert_true(events[0].is_active, "Start must publish IsActive = true");
    zassert_false(events[0].reason.has_value(), "an active status carries no reason");
    zassert_false(events[1].is_active, "Stop must publish IsActive = false");
    zassert_true(events[1].reason == LoggingStatusReason::Stopped, "a stop's reason is Stopped");

    zassert_true(rig.log_writer_service->IsStopped(), "the requested stop must leave the service stopped");
    zassert_false(rig.log_writer_service->HasFailed(), "a clean session has not failed");
}

// The volume fills up under a running session: the tick that fails publishes the failure and
// schedules nothing more, the supervisor stops the service on the configuration work queue, the
// file is closed and stays readable, and logging starts again once there is room.
ZTEST(log_writer_lifecycle, test_AFullVolumeStopsTheSessionThroughTheEvent) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    const auto probes = ProbeLoggerFactory::Make(rig.logger_factory);
    rig.log_writer_service->SetLoggerFactory(probes->AsFactory());
    const std::string filler_path = "session_fill.bin";

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    const auto first_probe = probes->Current();
    const auto file_path = std::string(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR) + "/"
        + rig.fs_service->ListFiles(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR).at(0);

    FillVolume(*rig.fs_service, filler_path);

    zassert_true(PublishFramesUntil(rig, *sensors[1], [&] {
        return rig.log_writer_service->HasFailed() && rig.log_writer_service->IsStopped();
    }, 5000), "a session on a full volume must end and be stopped");
    zassert_equal(first_probe->stop_thread.load(), ConfigQueueThread(rig),
        "the stop must run on the configuration work queue, not the writer thread");

    zassert_true(recorder.WaitForEventCount(3), "Start, the failure and the stop each publish a status");
    const auto events = recorder.GetEvents();
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "the failure must be published as WriteFailed");
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the supervisor's stop publishes Stopped");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_true(stats.write_errors >= 1, "the failed write must be counted");

    zassert_true(CanOpenEveryFileObject(*rig.fs_service, file_path), "the stop must have closed the log file");
    const auto content = ReadWholeFile(*rig.fs_service, file_path);
    zassert_true(IsUnfinalized(content), "a file that could not be finalized stays readable as unfinalized");

    RemoveFiller(*rig.fs_service, filler_path);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(4), "a start must succeed once the volume has room again");
    zassert_false(rig.log_writer_service->HasFailed(), "Start must clear the failure");
    k_msleep(250);
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(5), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 2, "the failed file and the new one (found %zu)", files.size());
    AssertFinalizedFile(files[1].content, "the file after the restart");
}

// A record the logger cannot write ends the session at that tick: the error is counted, the
// failure published once, no tick follows, and the supervisor's stop finalizes the file, whose
// stream is intact here.
ZTEST(log_writer_lifecycle, test_AWriteThatThrowsEndsTheSessionAtThatTick) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors, false);
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    auto probe = std::make_shared<ProbeLogger>(rig.logger_factory());
    rig.log_writer_service->SetLoggerFactory([probe] { return probe; });
    probe->refuse_next_reading = true;

    PublishValue(rig, *sensors[0], 3, rig.time_service->GetCurrentTime());
    supervisor.RequestStart(ILoggingControl::kAllUnits);

    zassert_true(recorder.WaitForEventCount(3), "Start, the failure and the stop each publish a status");
    zassert_true(WaitFor([&] {
        return rig.log_writer_service->HasFailed() && rig.log_writer_service->IsStopped();
    }, 2000), "the failed session must be stopped by the supervisor");
    zassert_equal(probe->stop_thread.load(), ConfigQueueThread(rig),
        "the stop must run on the configuration work queue, not the writer thread");

    k_msleep(350);
    zassert_equal(probe->tick_times.size(), 1, "no tick may follow the one that failed (%zu ticks)", probe->tick_times.size());

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.write_errors, 1, "the refused record is one error (got %u)", stats.write_errors);

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 3, "the failure is published once (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "a refused record is published as WriteFailed");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());
    AssertFinalizedFile(files[0].content, "the file of the failed session");
}

ZTEST(log_writer_lifecycle, test_StartFailsWithoutTheMinimumFreeSpace) {
    auto rig = MakeLogWriterRig();
    const uint64_t total_bytes = rig.fs_service->GetTotalSpace();

    rig.log_writer_service->SetSizeLimitsForTesting(0, total_bytes + 1);
    zassert_false(rig.log_writer_service->Start(), "Start must fail when the volume has less than the minimum free");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "a refused Start must not create a file");

    rig.log_writer_service->SetSizeLimitsForTesting(0, total_bytes / 2);
    zassert_true(rig.log_writer_service->Start(), "Start must succeed with the minimum free");
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");
}

// A file that reaches the size limit is finalized and the next one opened at the start of the next
// tick: every file is finalized, they start one after the other, and every frame is in exactly one
// of them, in order.
ZTEST(log_writer_lifecycle, test_RotationAtTheSizeLimitKeepsEveryFrame) {
    constexpr uint8_t published_frames = 120;

    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    // About three ticks of the frames below per file.
    rig.log_writer_service->SetSizeLimitsForTesting(GetHeaderSize(rig) + 1024, 0);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    for(uint8_t i = 0; i < published_frames; i++) {
        PublishReadings(rig, sensors, i);
        k_msleep(10);
    }
    k_msleep(250);
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped(), "the requested stop must leave the service stopped");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.write_errors, 0, "nothing may fail on an empty volume");
    zassert_true(stats.rotations >= 2, "1.2 s of frames over a 1 KiB limit must rotate at least twice (got %u)", stats.rotations);
    zassert_equal(stats.frames, published_frames, "every frame is logged across the files (got %u)", stats.frames);
    zassert_false(rig.log_writer_service->HasFailed(), "a rotation is no failure");

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 2, "a rotation publishes nothing (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::Stopped, "only the requested stop ends the session");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), stats.rotations + 1, "one file per rotation and the first (found %zu)", files.size());

    std::vector<uint8_t> sequences;
    double last_frame_time_ns = 0.0;
    uint64_t total_bytes = 0;

    for(size_t i = 0; i < files.size(); i++) {
        AssertFinalizedFile(files[i].content, files[i].path.c_str());
        total_bytes += files[i].content.size();

        if(i > 0)
            zassert_true(files[i].start_time_ns > files[i - 1].start_time_ns, "file %zu must start after file %zu", i, i - 1);

        const auto records = ReadDataRecords(files[i].content);
        for(size_t j = 0; j < records.frame_times.size(); j++) {
            const double frame_time_ns = static_cast<double>(files[i].start_time_ns) + records.frame_times[j] * 1e9;
            zassert_true(frame_time_ns > last_frame_time_ns, "frame %zu of file %zu goes back in time", j, i);

            last_frame_time_ns = frame_time_ns;
            sequences.push_back(records.frame_sequences[j]);
        }
    }

    zassert_equal(sequences.size(), published_frames, "every frame must be in exactly one file (found %zu)", sequences.size());
    for(size_t i = 0; i < sequences.size(); i++)
        zassert_equal(sequences[i], i, "frame %zu is missing, repeated or out of order across the rotations", i);

    zassert_equal(stats.bytes, total_bytes, "the byte count must cover every file (%llu, files %llu)",
        static_cast<unsigned long long>(stats.bytes), static_cast<unsigned long long>(total_bytes));
}

// The free-space minimum is checked again before each rotation: without it the session ends with
// NoSpace, and the stop that follows finalizes the full file in the room that is left.
ZTEST(log_writer_lifecycle, test_ARotationWithoutTheMinimumFreeSpaceEndsWithNoSpace) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    const auto probes = ProbeLoggerFactory::Make(rig.logger_factory);
    rig.log_writer_service->SetLoggerFactory(probes->AsFactory());
    const std::string filler_path = "rotation_fill.bin";

    rig.log_writer_service->SetSizeLimitsForTesting(GetHeaderSize(rig) + 512, 128 * 1024);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must succeed on a volume with the minimum free");
    FillVolume(*rig.fs_service, filler_path, 96 * 1024);

    zassert_true(PublishFramesUntil(rig, *sensors[1], [&] {
        return rig.log_writer_service->HasFailed() && rig.log_writer_service->IsStopped();
    }, 5000), "a rotation without the minimum free must end the session, and the supervisor stop it");
    zassert_equal(probes->Current()->stop_thread.load(), ConfigQueueThread(rig),
        "the stop must run on the configuration work queue");

    zassert_true(recorder.WaitForEventCount(3), "Start, the failure and the stop each publish a status");
    const auto events = recorder.GetEvents();
    zassert_true(events[1].reason == LoggingStatusReason::NoSpace, "the failure must be published as NoSpace");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.rotations, 0, "no rotation may happen without the minimum free");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "no next file may be opened (found %zu)", files.size());
    AssertFinalizedFile(files[0].content, "the full file");

    RemoveFiller(*rig.fs_service, filler_path);
}

// A stored logging configuration restarts a running writer on the configuration work queue, and the
// next file is laid out for it: the factory runs at every Start(). Update() and an SMP-style
// ApplyCborConfiguration() both call the supervisor's handler.
ZTEST(log_writer_lifecycle, test_ALoggingConfigurationApplyRestartsTheWriter) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    k_msleep(250);

    StoreLoggingConfiguration(rig, 50);
    zassert_true(recorder.WaitForEventCount(3), "storing a configuration must restart the writer");
    k_msleep(250);

    const auto stored_cbor = rig.logging_configuration_manager->GetCborConfiguration();
    zassert_true(rig.logging_configuration_manager->ApplyCborConfiguration(stored_cbor), "the stored CBOR must apply");
    zassert_true(recorder.WaitForEventCount(5), "applying a configuration must restart the writer");
    k_msleep(250);

    zassert_true(rig.log_writer_service->IsRunning(), "the restarted writer must run");
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(6), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 6, "a stop and a start per stored configuration (%zu events)", events.size());
    for(size_t i = 0; i < events.size(); i++) {
        zassert_equal(events[i].is_active, i % 2 == 0, "event %zu must alternate between started and stopped", i);
        if(!events[i].is_active)
            zassert_true(events[i].reason == LoggingStatusReason::Stopped, "event %zu must be a clean stop", i);
    }

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 3, "one file per start (found %zu)", files.size());

    const char* intervals[] = {"100", "50", "50"};
    for(size_t i = 0; i < files.size(); i++) {
        AssertFinalizedFile(files[i].content, files[i].path.c_str());

        const std::string property = std::string("<e name=\"logging_interval_ms\">") + intervals[i] + "</e>";
        zassert_true(ContainsText(files[i].content, property), "file %zu must be written for a %s ms interval", i, intervals[i]);
    }
}

// The frame queues kRawCanQueueSize raw frames between two ticks; more are dropped, and counted.
ZTEST(log_writer_lifecycle, test_RawFramesPastTheQueueSizeAreCountedAsDropped) {
    constexpr size_t extra_frames = 5;

    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    StoreLoggingConfiguration(rig, 500);

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    for(size_t i = 0; i < SensorLimits::kRawCanQueueSize + extra_frames; i++)
        PublishFrame(rig, *sensors[1], static_cast<uint8_t>(i), rig.time_service->GetCurrentTime());

    k_msleep(800);
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.frames, SensorLimits::kRawCanQueueSize, "the queue holds %zu frames (logged %u)",
        SensorLimits::kRawCanQueueSize, stats.frames);
    zassert_equal(stats.frames_dropped, extra_frames, "the frames past the queue size are dropped (counted %u)",
        stats.frames_dropped);

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());

    const auto records = ReadDataRecords(files[0].content);
    zassert_equal(records.frame_sequences.size(), SensorLimits::kRawCanQueueSize, "one CAN record per queued frame");
    for(size_t i = 0; i < records.frame_sequences.size(); i++)
        zassert_equal(records.frame_sequences[i], i, "the oldest frames are kept, frame %zu is not", i);
}

// Stop() cancels synchronously: it waits for the tick that is writing, then finalizes the file with
// that tick's record in it.
ZTEST(log_writer_lifecycle, test_StopWaitsForARunningTickAndFinalizes) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);

    auto probe = std::make_shared<ProbeLogger>(rig.logger_factory());
    rig.log_writer_service->SetLoggerFactory([probe] { return probe; });
    probe->hold_next_reading = true;

    PublishValue(rig, *sensors[0], 7, rig.time_service->GetCurrentTime());
    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    zassert_equal(k_sem_take(&probe->held_reading_started, K_SECONDS(2)), 0, "the first tick must log the published value");

    const int64_t stop_start_ms = k_uptime_get();
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");
    const int64_t stop_ms = k_uptime_get() - stop_start_ms;

    zassert_true(probe->is_held_reading_done.load(), "Stop must not return before the running tick finished");
    zassert_true(probe->stop_found_held_reading_done.load(), "the file must be finalized after the running tick finished");
    zassert_true(stop_ms >= ProbeLogger::kHoldMs / 2, "Stop returned after %lld ms, before the tick could finish",
        static_cast<long long>(stop_ms));

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());
    AssertFinalizedFile(files[0].content, "the file");

    const auto records = ReadDataRecords(files[0].content);
    zassert_equal(records.value_times.size(), 1, "the record of the running tick must be in the file (found %zu)",
        records.value_times.size());
}
