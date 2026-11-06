#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
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

namespace {

constexpr size_t ID_BLOCK_SIZE = 64;

struct FileClock {
    int64_t start_ms;
    uint8_t time_class;
    bool says_set;
    bool says_uptime;
};

// The HD block's start (ms) and time class, and the file's `clock` property.
FileClock ReadFileClock(FsService& fs_service, const std::string& name) {
    const auto bytes = ReadWholeFile(fs_service, std::string(LogFileNaming::LOGS_DIR) + "/" + name);
    zassert_true(bytes.size() > ID_BLOCK_SIZE + 24, "the file holds no header");
    zassert_equal(std::string(bytes.begin() + ID_BLOCK_SIZE, bytes.begin() + ID_BLOCK_SIZE + 4), "##HD");

    uint64_t link_count = 0;
    std::memcpy(&link_count, bytes.data() + ID_BLOCK_SIZE + 16, sizeof(link_count));
    const size_t fields = ID_BLOCK_SIZE + 24 + static_cast<size_t>(link_count) * 8;

    uint64_t start_ns = 0;
    std::memcpy(&start_ns, bytes.data() + fields, sizeof(start_ns));

    const std::string text(bytes.begin(), bytes.end());

    return FileClock{ .start_ms = static_cast<int64_t>(start_ns / 1'000'000ULL),
        .time_class = bytes[fields + 8 + 2 + 2 + 1],
        .says_set = text.find("<e name=\"clock\">set</e>") != std::string::npos,
        .says_uptime = text.find("<e name=\"clock\">uptime</e>") != std::string::npos };
}

// The `YYYYMMDDTHHMMSS` of a file name, as Unix seconds.
int64_t NameSeconds(const std::string& name) {
    const auto at = name.find('T') - 8;
    std::tm time_utc{};
    time_utc.tm_year = std::stoi(name.substr(at, 4)) - 1900;
    time_utc.tm_mon = std::stoi(name.substr(at + 4, 2)) - 1;
    time_utc.tm_mday = std::stoi(name.substr(at + 6, 2));
    time_utc.tm_hour = std::stoi(name.substr(at + 9, 2));
    time_utc.tm_min = std::stoi(name.substr(at + 11, 2));
    time_utc.tm_sec = std::stoi(name.substr(at + 13, 2));

    return static_cast<int64_t>(timegm(&time_utc));
}

constexpr int64_t DAY_MS = 24LL * 3600 * 1000;

} // namespace

ZTEST(log_end_times, test_AStartAfterASetIsOnRealTime) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);

    const int64_t wall_ms = NowMs(rig) + 400 * DAY_MS;
    rig.time_service->SetWallClock(std::chrono::system_clock::time_point(std::chrono::milliseconds(wall_ms)));

    zassert_true(rig.log_writer_service->Start());
    k_msleep(350);
    zassert_true(rig.log_writer_service->Stop());

    const auto names = ListLogNames(*rig.fs_service);
    zassert_equal(names.size(), 1);

    const auto clock = ReadFileClock(*rig.fs_service, names[0]);
    zassert_true(clock.says_set && !clock.says_uptime, "the file says its start is real time");
    zassert_equal(clock.time_class, 10, "an external time source");
    zassert_true(clock.start_ms >= wall_ms && clock.start_ms < wall_ms + 5000, "the HD start is real time");
    zassert_equal(NameSeconds(names[0]), clock.start_ms / 1000, "the name is the start on the file's clock");

    const auto end_ms = end_times->Find(names[0]);
    zassert_true(end_ms.has_value());
    zassert_true(*end_ms >= clock.start_ms && *end_ms < clock.start_ms + 5000, "end_ms is on the file's clock");
}

ZTEST(log_end_times, test_AStartBeforeASetStaysOnTheUnitsClock) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);

    zassert_true(rig.log_writer_service->Start());
    const int64_t start_ms = NowMs(rig);
    k_msleep(150);
    rig.time_service->SetWallClock(
        std::chrono::system_clock::time_point(std::chrono::milliseconds(start_ms + 400 * DAY_MS))
    );
    k_msleep(200);
    zassert_true(rig.log_writer_service->Stop());

    const auto names = ListLogNames(*rig.fs_service);
    zassert_equal(names.size(), 1);

    const auto clock = ReadFileClock(*rig.fs_service, names[0]);
    zassert_true(clock.says_uptime && !clock.says_set, "a set while logging changes nothing in the file");
    zassert_equal(clock.time_class, 0);
    zassert_true(clock.start_ms >= start_ms - 1000 && clock.start_ms <= start_ms + 1000, "the HD start is unit time");
    zassert_equal(NameSeconds(names[0]), clock.start_ms / 1000);

    const auto end_ms = end_times->Find(names[0]);
    zassert_true(end_ms.has_value());
    zassert_true(*end_ms < start_ms + 5000, "end_ms stays on the unit's clock");
}

ZTEST(log_end_times, test_ARotationKeepsTheClockOfItsStart) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const auto end_times = AttachEndTimes(rig);

    const int64_t wall_ms = NowMs(rig) + 400 * DAY_MS;
    rig.time_service->SetWallClock(std::chrono::system_clock::time_point(std::chrono::milliseconds(wall_ms)));

    rig.log_writer_service->SetSizeLimitsForTesting(2048, 0);
    zassert_true(rig.log_writer_service->Start());

    // A later set must not reach the files of this start.
    rig.time_service->SetWallClock(
        std::chrono::system_clock::time_point(std::chrono::milliseconds(wall_ms + 400 * DAY_MS))
    );

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
    zassert_true(rig.log_writer_service->Stop());

    const auto names = ListLogNames(*rig.fs_service);
    zassert_true(names.size() >= 2);

    for(const auto& name : names) {
        const auto clock = ReadFileClock(*rig.fs_service, name);
        zassert_true(clock.says_set, "every file of the start is on real time");
        zassert_equal(clock.time_class, 10);
        zassert_true(clock.start_ms >= wall_ms && clock.start_ms < wall_ms + 60'000, "and on the first set's offset");
        zassert_equal(NameSeconds(name), clock.start_ms / 1000);

        const auto end_ms = end_times->Find(name);
        zassert_true(end_ms.has_value());
        zassert_true(*end_ms >= clock.start_ms && *end_ms < wall_ms + 60'000, "end_ms is on the same clock");
    }
}
