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
#include <utility>
#include <variant>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/logging_domain/loggers/logger_metadata.h"
#include "domain/logging_domain/loggers/mdf4_logger_sensor_reading.h"
#include "domain/logging_domain/services/log_writer_service.h"
#include "domain/logging_domain/services/log_writer_supervisor.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

// The log writer over a formatted FAT volume standing in for the SD card, supervised by the
// LogWriterSupervisor that ships. Without sensors the files it produces hold a header and nothing
// else.
namespace logging_test_support {

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::logging_domain::loggers::ILogger;
using eerie_leap::domain::logging_domain::loggers::LoggerMetadata;
using eerie_leap::domain::logging_domain::loggers::Mdf4LoggerSensorReading;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;
using eerie_leap::domain::logging_domain::models::SensorLoggingConfiguration;
using eerie_leap::domain::logging_domain::services::LogFileNaming;
using eerie_leap::domain::logging_domain::services::LogWriterService;
using eerie_leap::domain::logging_domain::services::LogWriterSupervisor;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::models::ReadingError;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::utilities::memory::Mrm;
using eerie_memory::make_unique_pmr;

// MDF layout the tests read back: identifier(8) at 0, ID block 64 bytes, then ##XX blocks whose
// length sits 8 bytes into their header.
constexpr size_t ID_BLOCK_SIZE = 64;
constexpr size_t BLOCK_HEADER_SIZE = 24;
constexpr size_t BLOCK_LENGTH_OFFSET = 8;
constexpr size_t HEADER_START_TIME_OFFSET = ID_BLOCK_SIZE + BLOCK_HEADER_SIZE + 6 * 8; // hd_start_time_ns

// The configuration work queue stops a failed session, which unwinds the exception of the finalize
// the dead stream refused. What that costs depends on the unwinder, not the word size: ARM's EHABI
// tables are light (1476 bytes measured on qemu_cortex_a9), libgcc's DWARF unwinder is not (6580 on
// qemu_malta, 7712 on qemu_cortex_a53, 8836 on qemu_riscv64). The stack comes from the heap without
// a guard, so an overflow corrupts the heap rather than faulting.
constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 16384;

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
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): fs_mount_t keeps the device name as a mutable pointer
            mount_point->storage_dev = const_cast<char*>(mount_point->mnt_point + 1);

        fs_service = std::make_shared<FsService>(mount_point);

        zassert_true(fs_service->Format(), "the test volume could not be formatted");
        zassert_true(fs_service->Initialize(), "the test volume could not be mounted");
    }

    return fs_service;
}

// The log files, and the configurations a test stored, which the next rig would otherwise load.
inline void RemoveDataDirectory() {
    const auto fs_service = GetFormattedFsService();

    if(fs_service->Exists(LogFileNaming::LOGS_DIR))
        fs_service->DeleteRecursive(LogFileNaming::LOGS_DIR);

    for(const char* path : { "config/logging_config.cbor", "config/sensors_config.cbor" }) {
        if(fs_service->Exists(path))
            fs_service->DeleteFile(path);
    }
}

inline std::vector<uint8_t> ReadWholeFile(FsService& fs_service, std::string_view relative_path) {
    const auto size = fs_service.GetFileSize(relative_path);
    zassert_true(size.has_value(), "the file size could not be read");

    std::vector<uint8_t> content(size.value());
    size_t read_size = 0;

    zassert_true(
        fs_service.ReadFile(relative_path, content.data(), content.size(), read_size), "the file could not be read"
    );
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
    size_t offset = ID_BLOCK_SIZE;

    while(offset + BLOCK_HEADER_SIZE <= data.size()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the block id is text inside a byte buffer
        const std::string id(reinterpret_cast<const char*>(data.data() + offset), 4);
        const auto length = ReadField<uint64_t>(data, offset + BLOCK_LENGTH_OFFSET);
        zassert_true(length >= BLOCK_HEADER_SIZE, "block %s has an invalid length", id.c_str());

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
    return !std::ranges::search(content, text).empty();
}

// A property of the header comment, as the logger writes LoggerMetadata.
inline std::string Property(const char* name, const std::string& value) {
    return std::string("<e name=\"") + name + "\">" + value + "</e>";
}

// A finalized file ends exactly where its DT block says it ends.
inline void AssertFinalizedFile(const std::vector<uint8_t>& content, const char* name) {
    zassert_true(content.size() >= ID_BLOCK_SIZE + BLOCK_HEADER_SIZE, "%s is shorter than an MDF header", name);
    zassert_true(IsFinalized(content), "%s must carry the finalized identifier", name);

    const auto data_block_address = FindDataBlockAddress(content);
    zassert_not_equal(data_block_address, 0, "%s has no DT block", name);

    const auto data_block_length = ReadField<uint64_t>(content, data_block_address + BLOCK_LENGTH_OFFSET);
    zassert_equal(
        content.size(), data_block_address + static_cast<size_t>(data_block_length),
        "%s must end where the patched DT length says it ends", name
    );
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

    if(!fs_service.Exists(LogFileNaming::LOGS_DIR))
        return files;

    for(const auto& name : fs_service.ListFiles(LogFileNaming::LOGS_DIR)) {
        LogFile file;
        file.path = std::string(LogFileNaming::LOGS_DIR) + "/" + name;
        file.content = ReadWholeFile(fs_service, file.path);
        zassert_true(
            file.content.size() >= HEADER_START_TIME_OFFSET + 8, "%s is shorter than an MDF header", file.path.c_str()
        );
        file.start_time_ns = static_cast<int64_t>(ReadField<uint64_t>(file.content, HEADER_START_TIME_OFFSET));
        files.push_back(std::move(file));
    }

    std::ranges::sort(files, {}, &LogFile::start_time_ns);

    return files;
}

inline size_t CountLogFiles(FsService& fs_service) {
    if(!fs_service.Exists(LogFileNaming::LOGS_DIR))
        return 0;

    return fs_service.ListFiles(LogFileNaming::LOGS_DIR).size();
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

    zassert_equal(
        max_free_bytes, 0, "the volume could not be filled down to %llu free bytes",
        static_cast<unsigned long long>(max_free_bytes)
    );

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
    const auto oil_pressure = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "oil_pressure");
    oil_pressure->metadata.name = "Oil pressure";
    oil_pressure->metadata.unit = "bar";
    oil_pressure->metadata.description = "Main gallery, <after> the filter & cooler";
    oil_pressure->configuration.type = SensorType::PHYSICAL_ANALOG;
    oil_pressure->configuration.channel = 0;
    oil_pressure->configuration.expression = "x * 0.1";

    const auto can_raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "can_raw");
    can_raw->metadata.name = "Raw frames";
    can_raw->configuration.type = SensorType::CANBUS_RAW;
    can_raw->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetDefaultPmr(), 0, 0x100);
    can_raw->configuration.UpdateConnectionString();

    return { oil_pressure, can_raw };
}

// Record layouts of MakeLoggedSensors(): record ids in creation order, sizes after the id.
constexpr uint8_t VALUE_RECORD_ID = 1; // time 8, value 4, raw value 4, invalidation 1
constexpr uint8_t VLSD_RECORD_ID = 2;  // length 4, payload
constexpr uint8_t CAN_RECORD_ID = 3;   // time 8, CAN_DataFrame 15
constexpr size_t VALUE_RECORD_SIZE = 1 + 8 + 4 + 4 + 1;
constexpr size_t CAN_RECORD_SIZE = 1 + 8 + 15;

struct DataRecords {
    std::vector<double> value_times;
    std::vector<float> values;
    std::vector<uint8_t> value_invalid_masks; // bit 0: value invalid, bit 1: raw value invalid
    std::vector<double> frame_times;
    std::vector<uint8_t> frame_sequences; // first payload byte
};

// Walks the records of the DT block of a file written for MakeLoggedSensors().
inline DataRecords ReadDataRecords(const std::vector<uint8_t>& content) {
    const auto data_block_address = FindDataBlockAddress(content);
    zassert_not_equal(data_block_address, 0, "the file has no DT block");

    const size_t end = data_block_address
        + static_cast<size_t>(ReadField<uint64_t>(content, data_block_address + BLOCK_LENGTH_OFFSET));
    size_t position = data_block_address + BLOCK_HEADER_SIZE;
    uint8_t last_sequence = 0;
    DataRecords records;

    while(position < end) {
        switch(content[position]) {
            case VALUE_RECORD_ID:
                records.value_times.push_back(ReadField<double>(content, position + 1));
                records.values.push_back(ReadField<float>(content, position + 1 + 8));
                records.value_invalid_masks.push_back(content[position + 1 + 8 + 4 + 4]);
                position += VALUE_RECORD_SIZE;
                break;
            case VLSD_RECORD_ID:
                last_sequence = content[position + 1 + 4];
                position += 1 + 4 + ReadField<uint32_t>(content, position + 1);
                break;
            case CAN_RECORD_ID:
                records.frame_times.push_back(ReadField<double>(content, position + 1));
                records.frame_sequences.push_back(last_sequence);
                position += CAN_RECORD_SIZE;
                break;
            default:
                zassert_unreachable("unknown record id %u at %zu", content[position], position);
        }
    }

    zassert_equal(position, end, "the last record must end where the DT block ends");

    return records;
}

// Seconds from the file's start time to @p time, as the master channel stores it.
inline double
SecondsSinceFileStart(const std::vector<uint8_t>& content, const std::chrono::system_clock::time_point& time) {
    const auto start_time_ns = static_cast<int64_t>(ReadField<uint64_t>(content, HEADER_START_TIME_OFFSET));
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

// What the supervisor's factory does at every start: a logger for the stored logging configuration's
// interval and size limit over @p sensors, here with every sensor enabled and its raw value logged.
// The logger takes a snapshot, so the sensors are enabled there without storing a configuration.
inline LogWriterService::LoggerFactory MakeLoggerFactory(
    const std::shared_ptr<LoggingConfigurationManager>& logging_configuration_manager,
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    bool log_only_new_data
) {

    return [logging_configuration_manager, sensors, log_only_new_data] -> std::shared_ptr<ILogger<SensorReading>> {
        const auto stored_configuration = logging_configuration_manager->Get();

        LoggingConfiguration logger_configuration(std::allocator_arg, Mrm::GetDefaultPmr());
        logger_configuration.logging_interval_ms = stored_configuration->logging_interval_ms;
        logger_configuration.max_log_size_mb = stored_configuration->max_log_size_mb;
        for(const auto& sensor : sensors) {
            logger_configuration.sensor_configurations.emplace(
                sensor->id_hash,
                SensorLoggingConfiguration{
                    .is_enabled = true, .log_raw_value = true, .log_only_new_data = log_only_new_data }
            );
        }

        return std::make_shared<Mdf4LoggerSensorReading>(logger_configuration, sensors, MakeTestLoggerMetadata());
    };
}

// The tool ID the supervisor is given here: not MakeTestLoggerMetadata()'s, so a header written by
// the rig's own factory cannot pass for one written by the supervisor's.
inline constexpr const char* TEST_TOOL_ID = "rt_core_supervisor";

// Carries logging_events in this binary. The channel drops every publish until a bus registers it,
// and stays with the first one, so every rig shares this bus.
class LoggingTestBus : public EventBus {
private:
    static constexpr int STACK_SIZE = 4096;

    LoggingTestBus() :
        EventBus("logging_test_bus", STACK_SIZE) {
        RegisterChannel(LoggingEventsChannel::GetInstance());
    }

public:
    static LoggingTestBus& Get() {
        static LoggingTestBus bus;

        return bus;
    }
};

// The service of the last rig, held until ResetAfterTest() has stopped it. A writer destroyed while
// it runs leaves its tick armed on freed memory, and one left by a test that an assertion aborted
// would go on writing into the next test's directory.
inline std::shared_ptr<LogWriterService>& LastRigService() {
    static std::shared_ptr<LogWriterService> service;

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
    // The one the supervisor builds its loggers from; see AttachSupervisor().
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager;
};

// Default logging configuration (100 ms), initialized, not started. The frame is laid out for
// @p sensors, and the logger logs every one of them with its raw value.
inline LogWriterRig
MakeLogWriterRig(std::span<const std::shared_ptr<Sensor>> sensors = {}, bool log_only_new_data = true) {
    LogWriterRig rig;

    LoggingTestBus::Get();

    rig.fs_service = GetFormattedFsService();

    rig.config_work_queue = std::make_shared<WorkQueueThread>("config_work_queue", CONFIG_WORK_QUEUE_STACK_SIZE, 5);
    zassert_true(rig.config_work_queue->Initialize(), "the configuration work queue did not start");

    rig.logging_configuration_manager = std::make_shared<LoggingConfigurationManager>(
        std::make_unique<CborConfigurationService<CborLoggingConfig>>(
            "logging_config", rig.fs_service, rig.config_work_queue
        )
    );
    zassert_not_null(rig.logging_configuration_manager->Get().get(), "no logging configuration is available");

    rig.time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );
    rig.frame = std::make_shared<SensorReadingsFrame>();
    rig.frame->Configure(sensors);

    rig.log_writer_service = std::make_shared<LogWriterService>(
        rig.fs_service, rig.logging_configuration_manager, rig.time_service, rig.frame
    );
    zassert_true(rig.log_writer_service->Initialize(), "the log writer service did not initialize");

    rig.logger_factory = MakeLoggerFactory(
        rig.logging_configuration_manager, std::vector<std::shared_ptr<Sensor>>(sensors.begin(), sensors.end()),
        log_only_new_data
    );
    rig.log_writer_service->SetLoggerFactory(rig.logger_factory);
    LastRigService() = rig.log_writer_service;

    return rig;
}

// For code off the test thread, which must not assert: an assertion there aborts that thread.
inline bool
TryStoreLoggingConfiguration(LoggingConfigurationManager& manager, uint32_t interval_ms, uint32_t max_log_size_mb = 0) {
    LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = interval_ms;
    configuration.max_log_size_mb = max_log_size_mb;

    return manager.Update(configuration);
}

// Stores a logging configuration with @p interval_ms and @p max_log_size_mb and no sensor entries;
// the rig's factory enables its sensors itself. Never while the queue is gated: the save runs there.
inline void StoreLoggingConfiguration(const LogWriterRig& rig, uint32_t interval_ms, uint32_t max_log_size_mb = 0) {
    zassert_true(
        TryStoreLoggingConfiguration(*rig.logging_configuration_manager, interval_ms, max_log_size_mb),
        "the logging configuration was not stored"
    );
}

// Stores a logging configuration that logs @p sensors, for the supervisor's own factory.
inline void EnableSensorLogging(
    const LogWriterRig& rig,
    std::span<const std::shared_ptr<Sensor>> sensors,
    uint32_t interval_ms = 100
) {
    LoggingConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = interval_ms;
    configuration.max_log_size_mb = 0;
    for(const auto& sensor : sensors) {
        configuration.sensor_configurations.emplace(sensor->id_hash, SensorLoggingConfiguration{ .is_enabled = true });
    }

    zassert_true(rig.logging_configuration_manager->Update(configuration), "the logging configuration was not stored");
}

inline void PublishValue(
    const LogWriterRig& rig,
    const Sensor& sensor,
    uint8_t sequence,
    const std::chrono::system_clock::time_point& time
) {
    SensorReading value_reading(&sensor);
    value_reading.source = ReadingSource::PROCESSING;
    value_reading.status = ReadingStatus::PROCESSED;
    value_reading.timestamp = time;
    value_reading.raw_value = static_cast<float>(100 + sequence);
    value_reading.value = value_reading.raw_value.value() * 0.1F;
    rig.frame->AddOrUpdateReading(value_reading);
}

inline void PublishFrame(
    const LogWriterRig& rig,
    const Sensor& sensor,
    uint8_t sequence,
    const std::chrono::system_clock::time_point& time
) {
    CanFrame can_frame;
    can_frame.id = 0x100;
    can_frame.data = { sequence, 0x22, 0x33, 0x44 };

    SensorReading frame_reading(&sensor);
    frame_reading.source = ReadingSource::ISR;
    frame_reading.status = ReadingStatus::PROCESSED;
    frame_reading.timestamp = time;
    frame_reading.can_frame = &can_frame;
    rig.frame->AddOrUpdateReading(frame_reading);
}

// One processed value and one raw frame, stamped now, as the sensor pipeline commits them.
inline std::chrono::system_clock::time_point
PublishReadings(LogWriterRig& rig, const std::vector<std::shared_ptr<Sensor>>& sensors, uint8_t sequence) {

    const auto now = rig.time_service->GetCurrentTime();

    PublishValue(rig, *sensors[0], sequence, now);
    PublishFrame(rig, *sensors[1], sequence, now);

    return now;
}

// The supervisor of the running test. It lives here rather than in the rig: a test that an
// assertion aborts leaves its stack without unwinding, and ResetAfterTest() still withdraws it.
inline std::shared_ptr<LogWriterSupervisor>& SupervisorSlot() {
    static std::shared_ptr<LogWriterSupervisor> supervisor;

    return supervisor;
}

/**
 * Supervises the rig's writer, on the rig's configuration work queue, as a unit that logs does.
 *
 * @param sensors_configuration_manager What the supervisor's loggers describe; null builds one on
 *        sensors_config with the default, no sensors.
 * @param uses_own_logger_factory Keeps the supervisor's factory. Otherwise the rig's factory, which
 *        logs every sensor of the frame, replaces it.
 */
inline LogWriterSupervisor& AttachSupervisor(
    LogWriterRig& rig,
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager = nullptr,
    bool uses_own_logger_factory = false,
    LogWriterSupervisor::DeviceIdProvider device_id_provider = nullptr
) {

    if(sensors_configuration_manager == nullptr) {
        sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
            std::make_unique<CborConfigurationService<CborSensorsConfig>>(
                "sensors_config", rig.fs_service, rig.config_work_queue
            ),
            nullptr, 0, 0
        );
    }
    rig.sensors_configuration_manager = sensors_configuration_manager;

    const auto supervisor = std::make_shared<LogWriterSupervisor>(
        rig.config_work_queue, rig.log_writer_service, rig.logging_configuration_manager,
        std::move(sensors_configuration_manager), TEST_TOOL_ID, std::move(device_id_provider)
    );
    SupervisorSlot() = supervisor;
    zassert_true(supervisor->Initialize(), "the log writer supervisor did not initialize");

    if(!uses_own_logger_factory)
        rig.log_writer_service->SetLoggerFactory(rig.logger_factory);

    return *supervisor;
}

// Destroys the supervisor; references to it dangle afterwards.
inline void ReleaseSupervisor() {
    SupervisorSlot().reset();
}

// Holds a work queue in one job until Open(), so whatever is queued meanwhile runs after it, in
// order. Each Close() has its own context, so an Open() never leaves a token for the next one.
class ConfigQueueGate {
private:
    struct Context {
        k_sem entered{};
        k_sem open{};

        Context() {
            k_sem_init(&entered, 0, 1);
            k_sem_init(&open, 0, 1);
        }
    };

    std::shared_ptr<Context> context_;

public:
    // Returns once the queue runs the gate's job, so it is provably held. The job gives up after
    // 5 s, so a test that never opens it cannot wedge the queue for good.
    [[nodiscard]] bool Close(WorkQueueThread& work_queue) {
        Open();

        const auto context = std::make_shared<Context>();
        work_queue.Run([context] {
            k_sem_give(&context->entered);
            k_sem_take(&context->open, K_SECONDS(5));
        });
        context_ = context;

        return k_sem_take(&context->entered, K_SECONDS(5)) == 0;
    }

    // Does nothing while no gate is closed.
    void Open() {
        if(context_ == nullptr)
            return;

        k_sem_give(&context_->open);
        context_.reset();
    }
};

inline ConfigQueueGate& Gate() {
    static ConfigQueueGate gate;

    return gate;
}

// Returns once everything queued on the rig's configuration work queue before the call has run.
[[nodiscard]] inline bool FlushConfigQueue(const LogWriterRig& rig) {
    const auto done = std::make_shared<k_sem>();
    k_sem_init(done.get(), 0, 1);

    rig.config_work_queue->Run([done] {
        k_sem_give(done.get());
    });

    return k_sem_take(done.get(), K_SECONDS(5)) == 0;
}

inline k_tid_t ConfigQueueThread(const LogWriterRig& rig) {
    return k_work_queue_thread_get(rig.config_work_queue->GetWorkQueue());
}

struct StatusEvent {
    bool is_active = false;
    std::optional<LoggingStatusReason> reason;
};

// Records every StatusUpdated on logging_events, from its construction on.
class StatusRecorder {
private:
    AnySubscription subscription_;

    mutable k_mutex lock_{};
    std::vector<StatusEvent> events_;

    // The recorder of the running test; see ResetAfterTest().
    static StatusRecorder*& Active() {
        static StatusRecorder* recorder = nullptr;

        return recorder;
    }

    void OnStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
        StatusEvent status;

        const auto* const is_active = event.payload.find(LoggingPayloadType::IsActive);
        if(is_active != event.payload.end() && std::holds_alternative<bool>(is_active->second))
            status.is_active = std::get<bool>(is_active->second);

        const auto* const reason = event.payload.find(LoggingPayloadType::Reason);
        if(reason != event.payload.end() && std::holds_alternative<uint32_t>(reason->second))
            status.reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason->second));

        ScopedMutex guard(lock_);
        events_.push_back(status);
    }

public:
    StatusRecorder() {
        k_mutex_init(&lock_);

        LoggingTestBus::Get();

        subscription_ = CreateScopedSubscription(
            LoggingEventsChannel::GetInstance(), LoggingEventType::StatusUpdated,
            [this](const LoggingEventsChannel::EventMessage& event) {
                OnStatusUpdated(event);
            }
        );
        zassert_not_null(subscription_.get(), "the logging_events subscription was refused");

        Active() = this;
    }

    ~StatusRecorder() {
        Release();
    }

    StatusRecorder(const StatusRecorder&) = delete;
    StatusRecorder& operator=(const StatusRecorder&) = delete;

    void Release() {
        subscription_.reset();

        if(Active() == this)
            Active() = nullptr;
    }

    // A recorder left behind by a test that was aborted: its stack is gone, so its subscription must
    // go before the next event is dispatched to it.
    static void ReleaseLeftover() {
        if(Active() != nullptr)
            Active()->Release();
    }

    std::vector<StatusEvent> GetEvents() const {
        ScopedMutex guard(lock_);

        return events_;
    }

    bool WaitForEventCount(size_t count, int64_t timeout_ms = 2000) const {
        return WaitFor(
            [&] {
                return GetEvents().size() >= count;
            },
            timeout_ms
        );
    }
};

// Once armed, holds the bus thread inside the dispatch of the next inactive StatusUpdated until
// Unblock(), so a test can act while that dispatch is in flight. Subscribed before another
// subscriber, it runs before that one's handler.
class StatusBlocker {
private:
    // Owned by the handler, so a blocker on an aborted test's stack leaves nothing it could reach.
    struct Context {
        k_sem entered{};
        k_sem release{};
        std::atomic<bool> is_armed = false;

        Context() {
            k_sem_init(&entered, 0, 1);
            k_sem_init(&release, 0, 1);
        }
    };

    std::shared_ptr<Context> context_;
    AnySubscription subscription_;

    static StatusBlocker*& Active() {
        static StatusBlocker* blocker = nullptr;

        return blocker;
    }

public:
    StatusBlocker() :
        context_(std::make_shared<Context>()) {
        LoggingTestBus::Get();

        subscription_ = CreateScopedSubscription(
            LoggingEventsChannel::GetInstance(), LoggingEventType::StatusUpdated,
            [context = context_](const LoggingEventsChannel::EventMessage& event) {
                const auto* const is_active = event.payload.find(LoggingPayloadType::IsActive);
                if(is_active == event.payload.end()
                    || !std::holds_alternative<bool>(is_active->second)
                    || std::get<bool>(is_active->second)) {
                    return;
                }

                if(!context->is_armed.exchange(false))
                    return;

                k_sem_give(&context->entered);
                k_sem_take(&context->release, K_SECONDS(5));
            }
        );
        zassert_not_null(subscription_.get(), "the logging_events subscription was refused");

        Active() = this;
    }

    ~StatusBlocker() {
        Release();
    }

    StatusBlocker(const StatusBlocker&) = delete;
    StatusBlocker& operator=(const StatusBlocker&) = delete;

    void Arm() {
        context_->is_armed = true;
    }

    [[nodiscard]] bool WaitUntilBlocking(int32_t timeout_ms = 2000) {
        return k_sem_take(&context_->entered, K_MSEC(timeout_ms)) == 0;
    }

    void Unblock() {
        k_sem_give(&context_->release);
    }

    void Release() {
        Unblock();
        subscription_.reset();

        if(Active() == this)
            Active() = nullptr;
    }

    static void ReleaseLeftover() {
        if(Active() != nullptr)
            Active()->Release();
    }
};

// After each test. A test that passed has destroyed its rig, recorder and blocker already; a test
// that was aborted left them, and maybe a closed gate, behind.
inline void ResetAfterTest(void* = nullptr) {
    Gate().Open();
    StatusBlocker::ReleaseLeftover();
    StatusRecorder::ReleaseLeftover();
    ReleaseSupervisor();

    if(const auto service = std::exchange(LastRigService(), nullptr); service != nullptr)
        service->Stop();

    // The bus delivers what this test published before the next test subscribes.
    k_msleep(20);

    RemoveDataDirectory();
}

} // namespace logging_test_support
