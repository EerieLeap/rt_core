#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef CONFIG_ARCH_POSIX
#include <fstream>
#endif

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "domain/sensor_domain/models/sensor_limits.h"

#include "logging_test_support.h"

using namespace logging_test_support;
using eerie_leap::domain::sensor_domain::models::SensorLimits;

namespace {

// native_sim links the host libc, so the finalized file can be handed to asammdf on the host.
void DumpToHost([[maybe_unused]] const std::vector<uint8_t>& content) {
#ifdef CONFIG_ARCH_POSIX
    const std::string_view dump_dir = CONFIG_LOGGING_TEST_DUMP_DIR;
    if(dump_dir.empty())
        return;

    const std::string path = std::string(dump_dir) + "/logging_functional_dump.mf4";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    zassert_true(file.is_open(), "the dump file %s could not be opened", path.c_str());
    file.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    zassert_true(file.good(), "the dump file %s could not be written", path.c_str());
#endif
}

} // namespace

ZTEST_SUITE(log_writer_service, NULL, NULL, ResetAfterTest, NULL, NULL);

ZTEST(log_writer_service, test_StartAndStopProduceAFinalizedFile) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    zassert_equal(rig.logging_configuration_manager->Get()->logging_interval_ms, 100,
        "the default configuration ticks every 100 ms");

    constexpr uint8_t published_frames = 7;
    std::vector<std::chrono::system_clock::time_point> published_times;

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    for(uint8_t i = 0; i < published_frames; i++) {
        published_times.push_back(PublishReadings(rig, sensors, i));
        k_sleep(K_MSEC(50));
    }
    // One more tick drains the frames queued after the last one.
    k_sleep(K_MSEC(150));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_true(stats.ticks >= 4, "500 ms at 100 ms must run at least 4 ticks (got %u)", stats.ticks);
    zassert_equal(stats.write_errors, 0, "nothing may fail on an empty volume");
    zassert_equal(stats.frames, published_frames, "every queued raw frame is logged (got %u)", stats.frames);
    zassert_true(stats.readings >= 3, "each tick sees a new value timestamp (got %u readings)", stats.readings);

    const auto files = rig.fs_service->ListFiles(LogFileNaming::LOGS_DIR);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());

    const std::string path = std::string(LogFileNaming::LOGS_DIR) + "/" + files[0];
    const auto content = ReadWholeFile(*rig.fs_service, path);

    zassert_true(content.size() >= kIdBlockSize + kBlockHeaderSize, "the file is shorter than an MDF header");
    zassert_mem_equal(content.data(), "MDF     ", 8, "a stopped file must carry the finalized identifier");

    const auto data_block_address = FindDataBlockAddress(content);
    zassert_not_equal(data_block_address, 0, "the file has no DT block");

    const auto data_block_length = ReadField<uint64_t>(content, data_block_address + kBlockLengthOffset);
    zassert_equal(content.size(), data_block_address + static_cast<size_t>(data_block_length),
        "the file must end where the patched DT length says it ends");
    zassert_equal(stats.bytes, content.size(), "the logger's byte count must match the file size");

    // Two frames 50 ms apart are drained by one tick, and each keeps the time it was published at.
    const auto records = ReadDataRecords(content);
    zassert_equal(records.frame_times.size(), published_frames, "one CAN record per frame (got %zu)", records.frame_times.size());
    for(size_t i = 0; i < records.frame_times.size(); i++) {
        zassert_equal(records.frame_sequences[i], i, "frame %zu is out of order", i);

        const double expected = SecondsSinceFileStart(content, published_times[i]);
        zassert_within(records.frame_times[i], expected, 1e-6,
            "frame %zu at %f s, published at %f s after the start", i, records.frame_times[i], expected);
    }

    zassert_equal(records.value_times.size(), stats.readings, "one value record per logged reading");
    for(size_t i = 0; i < records.value_times.size(); i++) {
        const auto match = std::ranges::find_if(published_times, [&](const auto& time) {
            return std::abs(SecondsSinceFileStart(content, time) - records.value_times[i]) < 1e-6;
        });
        zassert_true(match != published_times.end(), "value %zu at %f s is no published sample time", i, records.value_times[i]);

        if(i > 0)
            zassert_true(records.value_times[i] > records.value_times[i - 1], "value times must strictly increase at %zu", i);
    }

    DumpToHost(content);
}

// Frames the frame queued while logging was stopped predate the file: they are not logged, and
// they do not take the queue places of the frames that arrive after the start.
ZTEST(log_writer_service, test_FramesQueuedBeforeStartAreNotLogged) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);

    for(size_t i = 0; i < SensorLimits::kRawCanQueueSize + 4; i++)
        PublishFrame(rig, *sensors[1], 0xF0, rig.time_service->GetCurrentTime());
    k_sleep(K_MSEC(20));

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");

    constexpr uint8_t published_frames = 3;
    for(uint8_t i = 0; i < published_frames; i++)
        PublishFrame(rig, *sensors[1], i, rig.time_service->GetCurrentTime());

    k_sleep(K_MSEC(250));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto stats = rig.log_writer_service->GetStats();
    zassert_equal(stats.frames, published_frames, "only the frames after the start are logged (got %u)", stats.frames);
    zassert_equal(stats.frames_dropped, 0, "drops before the start are not this file's (got %u)", stats.frames_dropped);

    const auto files = rig.fs_service->ListFiles(LogFileNaming::LOGS_DIR);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());

    const auto content = ReadWholeFile(*rig.fs_service, std::string(LogFileNaming::LOGS_DIR) + "/" + files[0]);
    const auto records = ReadDataRecords(content);
    zassert_equal(records.frame_times.size(), published_frames, "one CAN record per frame after the start (got %zu)",
        records.frame_times.size());
    for(size_t i = 0; i < records.frame_sequences.size(); i++)
        zassert_equal(records.frame_sequences[i], i, "frame %zu must be one published after the start", i);
}

// Without log_only_new_data a sample is held: written at its own time, then again at every tick.
ZTEST(log_writer_service, test_HeldSampleIsRepeatedAtEveryTick) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors, false);

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    const auto published_time = rig.time_service->GetCurrentTime();
    PublishValue(rig, *sensors[0], 5, published_time);

    k_sleep(K_MSEC(550));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto files = rig.fs_service->ListFiles(LogFileNaming::LOGS_DIR);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());

    const auto content = ReadWholeFile(*rig.fs_service, std::string(LogFileNaming::LOGS_DIR) + "/" + files[0]);
    const auto records = ReadDataRecords(content);

    zassert_true(records.value_times.size() >= 4, "550 ms at 100 ms must repeat the sample (got %zu records)",
        records.value_times.size());
    zassert_within(records.value_times[0], SecondsSinceFileStart(content, published_time), 1e-6,
        "the first record carries the sample's own time");

    for(size_t i = 0; i < records.value_times.size(); i++) {
        zassert_within(records.values[i], 10.5F, 1e-4F, "record %zu must hold the one sample", i);

        if(i > 0)
            zassert_true(records.value_times[i] > records.value_times[i - 1], "value times must strictly increase at %zu", i);
    }
}

ZTEST(log_writer_service, test_FailingSensorIsLoggedInvalidUntilItRecovers) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors, false);

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    PublishValue(rig, *sensors[0], 5, rig.time_service->GetCurrentTime());
    k_sleep(K_MSEC(250));

    // The pipeline commits a failed reading as it is, with its own time and no value.
    SensorReading failed(sensors[0].get());
    failed.source = ReadingSource::PROCESSING;
    failed.timestamp = rig.time_service->GetCurrentTime();
    failed.SetError(ReadingError::READER_FAILED);
    rig.frame->AddOrUpdateReading(failed);
    k_sleep(K_MSEC(250));

    PublishValue(rig, *sensors[0], 7, rig.time_service->GetCurrentTime());
    k_sleep(K_MSEC(250));
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    const auto files = rig.fs_service->ListFiles(LogFileNaming::LOGS_DIR);
    zassert_equal(files.size(), 1, "one Start must leave exactly one file (found %zu)", files.size());

    const auto content = ReadWholeFile(*rig.fs_service, std::string(LogFileNaming::LOGS_DIR) + "/" + files[0]);
    const auto records = ReadDataRecords(content);

    // valid (10.5) ... invalid (0, bits 0 and 1) ... valid again (10.7)
    size_t first_invalid = records.values.size();
    size_t last_invalid = 0;
    for(size_t i = 0; i < records.values.size(); i++) {
        if(records.value_invalid_masks[i] != 0) {
            first_invalid = std::min(first_invalid, i);
            last_invalid = i;
            zassert_equal(records.value_invalid_masks[i], 0x03, "a failed reading has neither a value nor a raw value (record %zu)", i);
            zassert_equal(records.values[i], 0.0F, "an invalid value is written as 0 (record %zu)", i);
        }
    }

    zassert_true(first_invalid > 0 && first_invalid < records.values.size(), "the failure must show up after the first good samples");
    zassert_true(last_invalid >= first_invalid + 1, "the failure is held while it lasts (records %zu..%zu)", first_invalid, last_invalid);
    zassert_within(records.values[first_invalid - 1], 10.5F, 1e-4F, "before the failure the good sample is logged");
    zassert_true(last_invalid + 1 < records.values.size(), "the recovered sensor must be logged after the failure");
    zassert_within(records.values[last_invalid + 1], 10.7F, 1e-4F, "after recovery the new sample is logged valid");
    zassert_equal(records.value_invalid_masks[last_invalid + 1], 0, "a recovered sample clears the bits");
}

ZTEST(log_writer_service, test_SyncFollowsTheConfiguredInterval) {
    static_assert(CONFIG_EERIE_LEAP_LOG_SYNC_INTERVAL_MS == 300, "the suite configures a 300 ms sync interval");

    auto rig = MakeLogWriterRig();

    zassert_true(rig.log_writer_service->Start(), "Start must succeed on a formatted volume with a logger");
    k_sleep(K_MSEC(1000));

    const auto stats = rig.log_writer_service->GetStats();
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");

    zassert_true(stats.ticks >= 9, "1000 ms at 100 ms must run at least 9 ticks (got %u)", stats.ticks);
    zassert_between_inclusive(stats.syncs, 2, 4,
        "1000 ms at a 300 ms interval is 3 syncs, give or take one tick (got %u)", stats.syncs);
    zassert_equal(stats.write_errors, 0, "a sync on an empty volume must not fail");
}

// The header is synced at Start(), so a volume that refuses writes fails the Start instead of
// a tick an interval later, and no empty file is left behind.
ZTEST(log_writer_service, test_StartFailsWhenTheHeaderCannotReachTheVolume) {
    auto rig = MakeLogWriterRig();
    const std::string filler_path = "start_fill.bin";

    // Created before the fill so that Start() gets as far as the file, not just the directory.
    zassert_true(rig.fs_service->CreateDirectory(LogFileNaming::LOGS_DIR),
        "the data directory could not be created on an empty volume");

    const std::vector<uint8_t> chunk(4096, 0xA5);
    int appends = 0;
    while(appends < 1024 && rig.fs_service->WriteFile(filler_path, chunk.data(), chunk.size(), true))
        appends++;
    zassert_true(appends > 0 && appends < 1024, "the volume did not fill up");

    zassert_false(rig.log_writer_service->Start(), "Start must fail when the header cannot be synced");

    const auto files = rig.fs_service->ListFiles(LogFileNaming::LOGS_DIR);
    zassert_equal(files.size(), 0, "a failed Start must not leave a file behind (found %zu)", files.size());

    if(!rig.fs_service->DeleteFile(filler_path))
        zassert_true(rig.fs_service->Format(), "the filler could neither be deleted nor formatted away");

    zassert_true(rig.log_writer_service->Start(), "Start must succeed once the volume has room again");
    zassert_true(rig.log_writer_service->Stop(), "Stop must finalize and close the file");
}
