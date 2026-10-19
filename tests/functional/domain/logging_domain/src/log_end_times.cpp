#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/logging_domain/services/log_end_times.h"

#include "logging_test_support.h"

using namespace logging_test_support;
using eerie_leap::domain::logging_domain::services::LogEndTimes;

namespace {

int64_t NowMs(const LogWriterRig& rig) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(rig.time_service->GetCurrentTime().time_since_epoch())
        .count();
}

std::string IndexPath() {
    return std::string(LogFileNaming::LOGS_DIR) + "/" + LogFileNaming::END_TIMES_FILE_NAME;
}

// The log files in the logs directory, without the end-time index.
std::vector<std::string> ListLogNames(const FsService& fs_service) {
    std::vector<std::string> names;

    for(const auto& name : fs_service.ListFiles(LogFileNaming::LOGS_DIR)) {
        if(name.ends_with(".mf4"))
            names.push_back(name);
    }

    return names;
}

std::shared_ptr<LogEndTimes> AttachEndTimes(const LogWriterRig& rig) {
    auto end_times = std::make_shared<LogEndTimes>(rig.fs_service);
    rig.log_writer_service->SetEndTimes(end_times);

    return end_times;
}

std::string ReadIndex(FsService& fs_service) {
    if(!fs_service.Exists(IndexPath()))
        return {};

    const auto content = ReadWholeFile(fs_service, IndexPath());

    return { content.begin(), content.end() };
}

} // namespace

ZTEST_SUITE(log_end_times, NULL, NULL, ResetAfterTest, NULL, NULL);

ZTEST(log_end_times, test_SessionInfoFollowsTheSession) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);

    zassert_false(rig.log_writer_service->GetSessionInfo().has_value(), "no session before Start()");

    const int64_t before_ms = NowMs(rig);
    zassert_true(rig.log_writer_service->Start(), "Start must succeed");
    const int64_t after_ms = NowMs(rig);

    const auto names = ListLogNames(*rig.fs_service);
    zassert_equal(names.size(), 1);

    const auto started = rig.log_writer_service->GetSessionInfo();
    zassert_true(started.has_value(), "a session runs after Start()");
    zassert_true(started->since_ms >= before_ms && started->since_ms <= after_ms, "since_ms is the start time");
    zassert_str_equal(started->file_name.data(), names[0].c_str(), "the open file's name, without directory");
    zassert_true(started->file_bytes > 0, "the header is counted from the start");

    PublishReadings(rig, sensors, 1);
    k_msleep(350);

    const auto running = rig.log_writer_service->GetSessionInfo();
    zassert_true(running.has_value());
    zassert_equal(running->since_ms, started->since_ms, "the session's start does not move");
    zassert_true(running->file_bytes >= started->file_bytes, "the bytes of the file only grow");
    zassert_true(
        rig.log_writer_service->IsOpenFile(std::string(LogFileNaming::LOGS_DIR) + "/" + running->file_name.data())
    );

    zassert_true(rig.log_writer_service->Stop());
    zassert_false(rig.log_writer_service->GetSessionInfo().has_value(), "no session after Stop()");
}

ZTEST(log_end_times, test_AnEndTimeIsRecordedAtStop) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);

    zassert_true(rig.log_writer_service->Start());
    const int64_t start_ms = NowMs(rig);
    k_msleep(350);

    const auto names = ListLogNames(*rig.fs_service);
    zassert_equal(names.size(), 1);
    zassert_false(end_times->Find(names[0]).has_value(), "an open file has no end yet");

    zassert_true(rig.log_writer_service->Stop());
    const int64_t stop_ms = NowMs(rig);

    const auto end_ms = end_times->Find(names[0]);
    zassert_true(end_ms.has_value(), "a finalized file's end is recorded");
    zassert_true(*end_ms >= start_ms - 1000 && *end_ms <= stop_ms, "the end lies within the session");

    // Persisted: a new index reads it back.
    LogEndTimes reloaded(rig.fs_service);
    reloaded.Load();
    zassert_equal(reloaded.Find(names[0]), end_ms, "the end time survives a reload");
}

ZTEST(log_end_times, test_AnEndTimeIsRecordedAtARotation) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);

    rig.log_writer_service->SetSizeLimitsForTesting(2048, 0);
    zassert_true(rig.log_writer_service->Start());

    const auto first_names = ListLogNames(*rig.fs_service);
    zassert_equal(first_names.size(), 1);

    uint8_t sequence = 0;
    zassert_true(
        WaitFor(
            [&] {
                for(int i = 0; i < 2; i++)
                    PublishFrame(rig, *sensors[1], sequence++, rig.time_service->GetCurrentTime());

                return rig.log_writer_service->GetStats().rotations >= 1;
            },
            5000
        ),
        "the frames must reach the size limit"
    );

    zassert_true(end_times->Find(first_names[0]).has_value(), "the rotated file's end is recorded at once");

    const auto names = ListLogNames(*rig.fs_service);
    zassert_true(names.size() >= 2);
    const auto info = rig.log_writer_service->GetSessionInfo();
    zassert_true(info.has_value());

    const std::string open_name = info->file_name.data();
    zassert_true(rig.log_writer_service->Stop());
    zassert_true(end_times->Find(open_name).has_value(), "the last file's end is recorded at Stop()");
}

ZTEST(log_end_times, test_AFileThatCouldNotBeFinalizedHasNoEndTime) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);
    const std::string filler_path = "end_times_fill.bin";

    zassert_true(rig.log_writer_service->Start());
    const auto names = ListLogNames(*rig.fs_service);
    zassert_equal(names.size(), 1);

    FillVolume(*rig.fs_service, filler_path);

    uint8_t sequence = 0;
    zassert_true(
        WaitFor(
            [&] {
                PublishFrame(rig, *sensors[1], sequence++, rig.time_service->GetCurrentTime());

                return rig.log_writer_service->HasFailed();
            },
            5000
        ),
        "a full volume must fail the session"
    );
    zassert_true(
        rig.log_writer_service->GetSessionInfo().has_value(), "a failed session still runs until it is stopped"
    );

    zassert_true(rig.log_writer_service->Stop());
    zassert_false(rig.log_writer_service->GetSessionInfo().has_value());

    RemoveFiller(*rig.fs_service, filler_path);

    zassert_false(end_times->Find(names[0]).has_value(), "a file left unfinalized gets no end time");
    zassert_equal(end_times->Size(), 0);
}

ZTEST(log_end_times, test_LoadDropsTheEntriesOfDeletedLogsAndRewritesTheFile) {
    const auto fs_service = GetFormattedFsService();
    zassert_true(fs_service->CreateDirectory(LogFileNaming::LOGS_DIR));

    const std::vector<uint8_t> content(8, 0x5A);
    for(const char* name : { "a.mf4", "b.mf4", "c.mf4" })
        zassert_true(fs_service->WriteFile(std::string(LogFileNaming::LOGS_DIR) + "/" + name, content.data(), 8));

    {
        LogEndTimes end_times(fs_service);
        end_times.Record("a.mf4", 1000);
        end_times.Record("b.mf4", 2000);
        end_times.Record("c.mf4", 3000);
        end_times.Record("c.mf4", 3500);
        end_times.Record("gone.mf4", 4000);
    }

    zassert_true(fs_service->DeleteFile(std::string(LogFileNaming::LOGS_DIR) + "/b.mf4"));

    LogEndTimes reloaded(fs_service);
    reloaded.Load();

    zassert_equal(reloaded.Find("a.mf4"), 1000);
    zassert_false(reloaded.Find("b.mf4").has_value(), "the entry of a deleted log is dropped");
    zassert_false(reloaded.Find("gone.mf4").has_value(), "the entry of a log that never existed is dropped");
    zassert_equal(reloaded.Find("c.mf4"), 3500, "the last record of a name wins");
    zassert_equal(reloaded.Size(), 2);

    zassert_str_equal(ReadIndex(*fs_service).c_str(), "1000 a.mf4\n3500 c.mf4\n", "the file is rewritten pruned");

    // Never serves the index as a log.
    zassert_false(fs_service->ListFiles(LogFileNaming::LOGS_DIR).empty());
}

ZTEST(log_end_times, test_ACorruptFileStartsWhatCanBeRead) {
    const auto fs_service = GetFormattedFsService();
    zassert_true(fs_service->CreateDirectory(LogFileNaming::LOGS_DIR));

    const std::vector<uint8_t> content(8, 0x5A);
    zassert_true(fs_service->WriteFile(std::string(LogFileNaming::LOGS_DIR) + "/a.mf4", content.data(), 8));

    const std::string corrupt = "garbage\n1000 a.mf4\n12x b.mf4\n\n-\n";
    zassert_true(fs_service->WriteFile(IndexPath(), corrupt.data(), corrupt.size()));

    LogEndTimes end_times(fs_service);
    end_times.Load();

    zassert_equal(end_times.Find("a.mf4"), 1000);
    zassert_equal(end_times.Size(), 1);
    zassert_str_equal(ReadIndex(*fs_service).c_str(), "1000 a.mf4\n", "the readable part is kept");

    LogEndTimes missing(GetFormattedFsService());
    fs_service->DeleteFile(IndexPath());
    missing.Load();
    zassert_equal(missing.Size(), 0, "a missing file is an empty index");
}

ZTEST(log_end_times, test_TheIndexKeepsTheNewestEntriesOnly) {
    const auto fs_service = GetFormattedFsService();
    zassert_true(fs_service->CreateDirectory(LogFileNaming::LOGS_DIR));

    LogEndTimes end_times(fs_service);
    constexpr size_t EXTRA = 5;
    for(size_t i = 0; i < LogEndTimes::MAX_ENTRIES + EXTRA; i++)
        end_times.Record("log_" + std::to_string(i) + ".mf4", static_cast<int64_t>(i));

    zassert_equal(end_times.Size(), LogEndTimes::MAX_ENTRIES, "the index is capped");
    zassert_false(end_times.Find("log_0.mf4").has_value(), "the oldest entries leave first");
    zassert_false(end_times.Find("log_4.mf4").has_value());
    zassert_equal(end_times.Find("log_5.mf4"), 5);
    zassert_equal(
        end_times.Find("log_" + std::to_string(LogEndTimes::MAX_ENTRIES + EXTRA - 1) + ".mf4"),
        static_cast<int64_t>(LogEndTimes::MAX_ENTRIES + EXTRA - 1)
    );
}
