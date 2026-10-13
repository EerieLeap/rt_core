#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "subsys/canbus/can_frame.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/event_bus/logging_event_bus.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/logging_domain/loggers/logger_metadata.h"
#include "domain/logging_domain/loggers/mdf4_logger_sensor_reading.h"
#include "domain/logging_domain/services/log_writer_service.h"

// The log writer wired the way rt_daq's LoggingController wires it, over a formatted FAT volume
// standing in for the SD card. Without sensors the files it produces hold a header and nothing else.
namespace logging_test_support {

using eerie_memory::make_unique_pmr;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingError;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventBus;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;
using eerie_leap::domain::logging_domain::models::SensorLoggingConfiguration;
using eerie_leap::domain::logging_domain::loggers::ILogger;
using eerie_leap::domain::logging_domain::loggers::LoggerMetadata;
using eerie_leap::domain::logging_domain::loggers::Mdf4LoggerSensorReading;
using eerie_leap::domain::logging_domain::services::LogWriterService;

// MDF layout the tests read back: identifier(8) at 0, ID block 64 bytes, then ##XX blocks whose
// length sits 8 bytes into their header.
constexpr size_t kIdBlockSize = 64;
constexpr size_t kBlockHeaderSize = 24;
constexpr size_t kBlockLengthOffset = 8;
constexpr size_t kHeaderStartTimeOffset = kIdBlockSize + kBlockHeaderSize + 6 * 8;   // hd_start_time_ns

// rt_daq's configuration work queue, where the stop of a failed session unwinds the finalize that
// the dead stream refused. 64-bit unwinding needs more than the 32-bit boards' 6144 bytes.
#ifdef CONFIG_64BIT
constexpr int kConfigWorkQueueStackSize = 16384;
#else
constexpr int kConfigWorkQueueStackSize = 6144;
#endif

// One volume for the suite, formatted on first use; the service is shared like the SD service is.
inline std::shared_ptr<FsService> GetFormattedFsService() {
    static std::shared_ptr<FsService> fs_service;

    if(fs_service == nullptr) {
        DtFs::InitInternalFs();
        auto* mount_point = DtFs::GetInternalFsMp();
        zassert_not_null(mount_point, "the overlay has no intfs0 mount");

        // The fatfs fstab entry leaves storage_dev empty, and fs_mkfs() wants the volume name
        // ("intfs0:"), which is the mount point without its leading slash.
        if(mount_point->storage_dev == nullptr)
            mount_point->storage_dev = const_cast<char*>(mount_point->mnt_point + 1);

        fs_service = std::make_shared<FsService>(mount_point);

        zassert_true(fs_service->Format(), "the test volume could not be formatted");
        zassert_true(fs_service->Initialize(), "the test volume could not be mounted");
    }

    return fs_service;
}

// The log files, and the configurations a test stored, which the next rig would otherwise load.
inline void RemoveDataDirectory() {
    auto fs_service = GetFormattedFsService();

    if(fs_service->Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR))
        fs_service->DeleteRecursive(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR);

    for(const char* path : {"config/logging_config.cbor", "config/sensors_config.cbor"}) {
        if(fs_service->Exists(path))
            fs_service->DeleteFile(path);
    }
}

inline std::vector<uint8_t> ReadWholeFile(FsService& fs_service, std::string_view relative_path) {
    const auto size = fs_service.GetFileSize(relative_path);
    zassert_true(size.has_value(), "the file size could not be read");

    std::vector<uint8_t> content(size.value());
    size_t read_size = 0;

    zassert_true(fs_service.ReadFile(relative_path, content.data(), content.size(), read_size),
        "the file could not be read");
    content.resize(read_size);

    return content;
}

template<typename T>
T ReadField(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    zassert_true(offset + sizeof(T) <= data.size(), "read past the end of the file");
    std::memcpy(&value, data.data() + offset, sizeof(T));

    return value;
}

// Walks the block chain from the end of the ID block; 0 when there is no DT block.
inline size_t FindDataBlockAddress(const std::vector<uint8_t>& data) {
    size_t offset = kIdBlockSize;

    while(offset + kBlockHeaderSize <= data.size()) {
        const std::string id(reinterpret_cast<const char*>(data.data() + offset), 4);
        const auto length = ReadField<uint64_t>(data, offset + kBlockLengthOffset);
        zassert_true(length >= kBlockHeaderSize, "block %s has an invalid length", id.c_str());

        if(id == "##DT")
            return offset;

        offset += static_cast<size_t>(length);
    }

    return 0;
}

inline bool IsFinalized(const std::vector<uint8_t>& content) {
    return content.size() >= 8 && std::memcmp(content.data(), "MDF     ", 8) == 0;
}

inline bool IsUnfinalized(const std::vector<uint8_t>& content) {
    return content.size() >= 8 && std::memcmp(content.data(), "UnFinMF ", 8) == 0;
}

inline bool ContainsText(const std::vector<uint8_t>& content, std::string_view text) {
    return std::search(content.begin(), content.end(), text.begin(), text.end()) != content.end();
}

// A finalized file ends exactly where its DT block says it ends.
inline void AssertFinalizedFile(const std::vector<uint8_t>& content, const char* name) {
    zassert_true(content.size() >= kIdBlockSize + kBlockHeaderSize, "%s is shorter than an MDF header", name);
    zassert_true(IsFinalized(content), "%s must carry the finalized identifier", name);

    const auto data_block_address = FindDataBlockAddress(content);
    zassert_not_equal(data_block_address, 0, "%s has no DT block", name);

    const auto data_block_length = ReadField<uint64_t>(content, data_block_address + kBlockLengthOffset);
    zassert_equal(content.size(), data_block_address + static_cast<size_t>(data_block_length),
        "%s must end where the patched DT length says it ends", name);
}

struct LogFile {
    std::string path;
    std::vector<uint8_t> content;
    int64_t start_time_ns = 0;
};

// Every file of the data directory, oldest first by the start time in its header: names started
// within the same second differ only in their random suffix.
inline std::vector<LogFile> ReadLogFiles(FsService& fs_service) {
    std::vector<LogFile> files;

    if(!fs_service.Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR))
        return files;

    for(const auto& name : fs_service.ListFiles(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR)) {
        LogFile file;
        file.path = std::string(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR) + "/" + name;
        file.content = ReadWholeFile(fs_service, file.path);
        zassert_true(file.content.size() >= kHeaderStartTimeOffset + 8, "%s is shorter than an MDF header", file.path.c_str());
        file.start_time_ns = static_cast<int64_t>(ReadField<uint64_t>(file.content, kHeaderStartTimeOffset));
        files.push_back(std::move(file));
    }

    std::ranges::sort(files, {}, &LogFile::start_time_ns);

    return files;
}

inline size_t CountLogFiles(FsService& fs_service) {
    if(!fs_service.Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR))
        return 0;

    return fs_service.ListFiles(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR).size();
}

// Polls @p condition every 10 ms until it holds or @p timeout_ms passed.
template<typename TCondition>
bool WaitFor(TCondition condition, int64_t timeout_ms) {
    const int64_t end = k_uptime_get() + timeout_ms;
    while(!condition()) {
        if(k_uptime_get() >= end)
            return false;

        k_msleep(10);
    }

    return true;
}

// Fills the volume with @p path until no more than @p max_free_bytes are left (0: until a write
// fails). The log writer may be writing meanwhile.
inline void FillVolume(FsService& fs_service, std::string_view path, uint64_t max_free_bytes = 0) {
    const std::vector<uint8_t> chunk(4096, 0xA5);

    for(int i = 0; i < 1024; i++) {
        if(max_free_bytes > 0 && fs_service.GetTotalSpace() - fs_service.GetUsedSpace() <= max_free_bytes)
            return;

        if(!fs_service.WriteFile(path, chunk.data(), chunk.size(), true))
            break;
    }

    zassert_equal(max_free_bytes, 0, "the volume could not be filled down to %llu free bytes",
        static_cast<unsigned long long>(max_free_bytes));

    // The last clusters, which a 4 KiB write no longer fits.
    for(int i = 0; i < 64; i++) {
        if(!fs_service.WriteFile(path, chunk.data(), 512, true))
            break;
    }
}

inline void RemoveFiller(FsService& fs_service, std::string_view path) {
    if(!fs_service.DeleteFile(path))
        zassert_true(fs_service.Format(), "the filler could neither be deleted nor formatted away");
}

// An analog sensor with a convertible expression and a raw CAN sensor on bus 0.
inline std::vector<std::shared_ptr<Sensor>> MakeLoggedSensors() {
    auto oil_pressure = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "oil_pressure");
    oil_pressure->metadata.name = "Oil pressure";
    oil_pressure->metadata.unit = "bar";
    oil_pressure->metadata.description = "Main gallery, <after> the filter & cooler";
    oil_pressure->configuration.type = SensorType::PHYSICAL_ANALOG;
    oil_pressure->configuration.channel = 0;
    oil_pressure->configuration.expression = "x * 0.1";

    auto can_raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "can_raw");
    can_raw->metadata.name = "Raw frames";
    can_raw->configuration.type = SensorType::CANBUS_RAW;
    can_raw->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 0, 0x100);
    can_raw->configuration.UpdateConnectionString();

    return {oil_pressure, can_raw};
}

// Record layouts of MakeLoggedSensors(): record ids in creation order, sizes after the id.
constexpr uint8_t kValueRecordId = 1;           // time 8, value 4, raw value 4, invalidation 1
constexpr uint8_t kVlsdRecordId = 2;            // length 4, payload
constexpr uint8_t kCanRecordId = 3;             // time 8, CAN_DataFrame 15
constexpr size_t kValueRecordSize = 1 + 8 + 4 + 4 + 1;
constexpr size_t kCanRecordSize = 1 + 8 + 15;

struct DataRecords {
    std::vector<double> value_times;
    std::vector<float> values;
    std::vector<uint8_t> value_invalid_masks;     // bit 0: value invalid, bit 1: raw value invalid
    std::vector<double> frame_times;
    std::vector<uint8_t> frame_sequences;   // first payload byte
};

// Walks the records of the DT block of a file written for MakeLoggedSensors().
inline DataRecords ReadDataRecords(const std::vector<uint8_t>& content) {
    const auto data_block_address = FindDataBlockAddress(content);
    zassert_not_equal(data_block_address, 0, "the file has no DT block");

    const size_t end = data_block_address + static_cast<size_t>(ReadField<uint64_t>(content, data_block_address + kBlockLengthOffset));
    size_t position = data_block_address + kBlockHeaderSize;
    uint8_t last_sequence = 0;
    DataRecords records;

    while(position < end) {
        switch(content[position]) {
        case kValueRecordId:
            records.value_times.push_back(ReadField<double>(content, position + 1));
            records.values.push_back(ReadField<float>(content, position + 1 + 8));
            records.value_invalid_masks.push_back(content[position + 1 + 8 + 4 + 4]);
            position += kValueRecordSize;
            break;
        case kVlsdRecordId:
            last_sequence = content[position + 1 + 4];
            position += 1 + 4 + ReadField<uint32_t>(content, position + 1);
            break;
        case kCanRecordId:
            records.frame_times.push_back(ReadField<double>(content, position + 1));
            records.frame_sequences.push_back(last_sequence);
            position += kCanRecordSize;
            break;
        default:
            zassert_unreachable("unknown record id %u at %zu", content[position], position);
        }
    }

    zassert_equal(position, end, "the last record must end where the DT block ends");

    return records;
}

// Seconds from the file's start time to @p time, as the master channel stores it.
inline double SecondsSinceFileStart(const std::vector<uint8_t>& content, const std::chrono::system_clock::time_point& time) {
    const auto start_time_ns = static_cast<int64_t>(ReadField<uint64_t>(content, kHeaderStartTimeOffset));
    const auto time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();

    return static_cast<double>(time_ns - start_time_ns) / 1e9;
}

inline LoggerMetadata MakeTestLoggerMetadata() {
    LoggerMetadata metadata;
    metadata.device_name = "functional_test_board";
    metadata.tool_id = "rt_core_functional";
    metadata.tool_version = "0.0.0";
    metadata.product_id = 50177;
    metadata.device_id = 3;
    metadata.sensors_configuration_hash = 0x12345678;

    return metadata;
}

// What rt_daq's factory does at every start: a logger for the stored logging configuration's
// interval and size limit over @p sensors, here with every sensor enabled and its raw value logged.
// The logger takes a snapshot, so the sensors are enabled there without storing a configuration.
inline LogWriterService::LoggerFactory MakeLoggerFactory(
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    std::vector<std::shared_ptr<Sensor>> sensors,
    bool log_only_new_data) {

    return [logging_configuration_manager, sensors, log_only_new_data]() -> std::shared_ptr<ILogger<SensorReading>> {
        const auto stored_configuration = logging_configuration_manager->Get();

        LoggingConfiguration logger_configuration(std::allocator_arg, Mrm::GetDefaultPmr());
        logger_configuration.logging_interval_ms = stored_configuration->logging_interval_ms;
        logger_configuration.max_log_size_mb = stored_configuration->max_log_size_mb;
        for(const auto& sensor : sensors) {
            logger_configuration.sensor_configurations.emplace(sensor->id_hash, SensorLoggingConfiguration {
                .is_enabled = true,
                .log_raw_value = true,
                .log_only_new_data = log_only_new_data
            });
        }

        return std::make_shared<Mdf4LoggerSensorReading>(logger_configuration, sensors, MakeTestLoggerMetadata());
    };
}

// The service of the last rig. A test that fails an assertion is aborted without unwinding, and its
// writer would go on writing into the next test's directory.
inline std::weak_ptr<LogWriterService>& LastRigService() {
    static std::weak_ptr<LogWriterService> service;

    return service;
}

struct LogWriterRig {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<WorkQueueThread> config_work_queue;
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager;
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    LogWriterService::LoggerFactory logger_factory;
    std::shared_ptr<LogWriterService> log_writer_service;
};

// Default logging configuration (100 ms), initialized, not started. The frame is laid out for
// @p sensors, and the logger logs every one of them with its raw value.
inline LogWriterRig MakeLogWriterRig(std::span<const std::shared_ptr<Sensor>> sensors = {}, bool log_only_new_data = true) {
    LogWriterRig rig;

    rig.fs_service = GetFormattedFsService();

    rig.config_work_queue = std::make_shared<WorkQueueThread>("config_work_queue", kConfigWorkQueueStackSize, 5);
    zassert_true(rig.config_work_queue->Initialize(), "the configuration work queue did not start");

    rig.logging_configuration_manager = std::make_shared<LoggingConfigurationManager>(
        std::make_unique<CborConfigurationService<CborLoggingConfig>>(
            "logging_config", rig.fs_service, rig.config_work_queue));
    zassert_not_null(rig.logging_configuration_manager->Get().get(), "no logging configuration is available");

    rig.time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
    rig.frame = std::make_shared<SensorReadingsFrame>();
    rig.frame->Configure(sensors);

    rig.log_writer_service = std::make_shared<LogWriterService>(
        rig.fs_service, rig.logging_configuration_manager, rig.time_service, rig.frame);
    zassert_true(rig.log_writer_service->Initialize(), "the log writer service did not initialize");

    rig.logger_factory = MakeLoggerFactory(
        rig.logging_configuration_manager,
        std::vector<std::shared_ptr<Sensor>>(sensors.begin(), sensors.end()),
        log_only_new_data);
    rig.log_writer_service->SetLoggerFactory(rig.logger_factory);
    LastRigService() = rig.log_writer_service;

    return rig;
}

// Stores a logging configuration with @p interval_ms and @p max_log_size_mb and no sensor entries;
// the rig's factory enables its sensors itself.
inline void StoreLoggingConfiguration(LogWriterRig& rig, uint32_t interval_ms, uint32_t max_log_size_mb = 0) {
    LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = interval_ms;
    configuration.max_log_size_mb = max_log_size_mb;

    zassert_true(rig.logging_configuration_manager->Update(configuration), "the logging configuration was not stored");
}

inline void PublishValue(LogWriterRig& rig, const Sensor& sensor, uint8_t sequence, const std::chrono::system_clock::time_point& time) {
    SensorReading value_reading(&sensor);
    value_reading.source = ReadingSource::PROCESSING;
    value_reading.status = ReadingStatus::PROCESSED;
    value_reading.timestamp = time;
    value_reading.raw_value = static_cast<float>(100 + sequence);
    value_reading.value = value_reading.raw_value.value() * 0.1F;
    rig.frame->AddOrUpdateReading(value_reading);
}

inline void PublishFrame(LogWriterRig& rig, const Sensor& sensor, uint8_t sequence, const std::chrono::system_clock::time_point& time) {
    CanFrame can_frame;
    can_frame.id = 0x100;
    can_frame.data = {sequence, 0x22, 0x33, 0x44};

    SensorReading frame_reading(&sensor);
    frame_reading.source = ReadingSource::ISR;
    frame_reading.status = ReadingStatus::PROCESSED;
    frame_reading.timestamp = time;
    frame_reading.can_frame = &can_frame;
    rig.frame->AddOrUpdateReading(frame_reading);
}

// One processed value and one raw frame, stamped now, as the sensor pipeline commits them.
inline std::chrono::system_clock::time_point PublishReadings(
    LogWriterRig& rig, const std::vector<std::shared_ptr<Sensor>>& sensors, uint8_t sequence) {

    const auto now = rig.time_service->GetCurrentTime();

    PublishValue(rig, *sensors[0], sequence, now);
    PublishFrame(rig, *sensors[1], sequence, now);

    return now;
}

struct StatusEvent {
    bool is_active = false;
    std::optional<LoggingStatusReason> reason;
};

// What an application does with logging_events, as rt_daq's LoggingController does it: a session
// that failed is stopped from the configuration work queue, never from the thread that published
// the event. Records every StatusUpdated event and where the stop ran.
class StatusMonitor {
private:
    std::shared_ptr<LogWriterService> service_;
    std::shared_ptr<WorkQueueThread> work_queue_;
    std::optional<WorkQueueTask<StatusMonitor>> stop_task_;
    AnySubscription subscription_;

    mutable k_mutex lock_;
    std::vector<StatusEvent> events_;

    // The monitor of the running test; see ResetAfterTest().
    static StatusMonitor*& Active() {
        static StatusMonitor* monitor = nullptr;

        return monitor;
    }

    static WorkQueueTaskResult ProcessStop(StatusMonitor* monitor) {
        monitor->stop_ran_on_work_queue = monitor->work_queue_->IsCurrentThread();
        monitor->stop_result = monitor->service_->Stop();
        monitor->stops++;

        return {};
    }

    void OnStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
        StatusEvent status;

        const auto is_active = event.payload.find(LoggingPayloadType::IsActive);
        if(is_active != event.payload.end() && std::holds_alternative<bool>(is_active->second))
            status.is_active = std::get<bool>(is_active->second);

        const auto reason = event.payload.find(LoggingPayloadType::Reason);
        if(reason != event.payload.end() && std::holds_alternative<uint32_t>(reason->second))
            status.reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason->second));

        {
            ScopedMutex guard(lock_);
            events_.push_back(status);
        }

        if(!status.is_active && status.reason.has_value() && status.reason.value() != LoggingStatusReason::Stopped)
            stop_task_->Reschedule(K_NO_WAIT);
    }

public:
    std::atomic<int> stops = 0;
    std::atomic<bool> stop_ran_on_work_queue = false;
    std::atomic<bool> stop_result = false;

    StatusMonitor(std::shared_ptr<LogWriterService> service, std::shared_ptr<WorkQueueThread> work_queue)
        : service_(std::move(service)), work_queue_(std::move(work_queue)) {

        k_mutex_init(&lock_);

        // The channel drops publishes until a bus carries it.
        LoggingEventBus::GetInstance();

        stop_task_ = work_queue_->CreateTask(ProcessStop, this);
        subscription_ = CreateScopedSubscription(
            LoggingEventsChannel::GetInstance(),
            LoggingEventType::StatusUpdated,
            [this](const LoggingEventsChannel::EventMessage& event) { OnStatusUpdated(event); });
        zassert_not_null(subscription_.get(), "the logging_events subscription was refused");

        Active() = this;
    }

    ~StatusMonitor() {
        Release();
    }

    void Release() {
        subscription_.reset();
        stop_task_->Cancel();

        if(Active() == this)
            Active() = nullptr;
    }

    // A monitor left behind by a test that was aborted: its stack is gone, so its subscription must
    // go before the next event is dispatched to it.
    static void ReleaseLeftover() {
        if(Active() != nullptr)
            Active()->Release();
    }

    StatusMonitor(const StatusMonitor&) = delete;
    StatusMonitor& operator=(const StatusMonitor&) = delete;

    std::vector<StatusEvent> GetEvents() const {
        ScopedMutex guard(lock_);

        return events_;
    }

    bool WaitForEventCount(size_t count, int64_t timeout_ms = 2000) const {
        return WaitFor([&] { return GetEvents().size() >= count; }, timeout_ms);
    }
};

// After each test. A test that passed has destroyed its rig and monitor already.
inline void ResetAfterTest(void* = nullptr) {
    StatusMonitor::ReleaseLeftover();

    if(auto service = LastRigService().lock())
        service->Stop();

    RemoveDataDirectory();
}

} // namespace logging_test_support
