#pragma once

// What the controllers suite shares for the script asset kinds of SensorsController and
// CanbusController. Only with CONFIG_EERIE_LEAP_ASSETS: functional.controllers.assets.
#ifdef CONFIG_EERIE_LEAP_ASSETS

    #include <atomic>
    #include <cstddef>
    #include <cstdint>
    #include <initializer_list>
    #include <memory>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    #include <zephyr/ztest.h>

    #include "subsys/assets/asset_kind.h"
    #include "subsys/assets/assets_manager.h"
    #include "subsys/device_tree/dt_fs.h"
    #include "subsys/fs/services/fs_service.h"
    #include "subsys/threading/work_queue_thread.h"
    #include "subsys/time/boot_elapsed_time_provider.h"
    #include "subsys/time/rtc_provider.h"
    #include "subsys/time/time_service.h"
    #include "utilities/memory/memory_resource_manager.h"

    #include "controllers/canbus_controller.h"
    #include "controllers/sensors_controller.h"
    #include "domain/canbus_domain/models/can_channel_configuration.h"
    #include "domain/canbus_domain/models/can_message_configuration.h"
    #include "domain/canbus_domain/models/canbus_configuration.h"
    #include "domain/sensor_domain/models/sensor.h"
    #include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

namespace script_assets_test {

using eerie_leap::controllers::CanbusController;
using eerie_leap::controllers::SensorsController;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;
using eerie_leap::domain::canbus_domain::models::CanMessageConfiguration;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::ToMask;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::utilities::memory::Mrm;

// A script a configuration refers to, and what the controllers register. The suite's one volume serves
// as the SD card and as the internal file system.
inline constexpr const char* SENSOR_SCRIPT = "scripts/sensors/a.lua";
inline constexpr const char* SENSOR_SCRIPT_NAME = "a.lua";
inline constexpr const char* DISABLED_SENSOR_SCRIPT = "scripts/sensors/d.lua";
inline constexpr const char* OTHER_SENSOR_SCRIPT_NAME = "other.lua";
// In the CAN scripts' directory, though a sensor refers to it.
inline constexpr const char* SENSOR_SCRIPT_IN_CAN_DIRECTORY = "scripts/can/c.lua";
inline constexpr const char* CAN_SCRIPT = "scripts/can/a.lua";
inline constexpr const char* CAN_SCRIPT_NAME = "a.lua";
inline constexpr const char* DISABLED_CAN_SCRIPT = "scripts/can/d.lua";
// In the sensor scripts' directory, though a CAN message refers to it.
inline constexpr const char* CAN_SCRIPT_IN_SENSOR_DIRECTORY = "scripts/sensors/s.lua";

inline constexpr uint8_t ALL_SCRIPT_ACCESS = ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE)
    | ToMask(AssetAccess::DELETE);

// The suite's volume as the SD card: every call goes to the real one, except IsAvailable(), which
// reports the switch. The controllers take it as their SD service, so a store built on the internal
// file system instead shows by not following the switch.
class SwitchableVolume final : public IFsService {
private:
    std::shared_ptr<IFsService> inner_;
    std::atomic<bool> is_available_ = true;

public:
    explicit SwitchableVolume(std::shared_ptr<IFsService> inner) :
        inner_(std::move(inner)) {}

    void SetAvailable(bool is_available) {
        is_available_ = is_available;
    }

    bool Initialize() override {
        return inner_->Initialize();
    }
    bool IsAvailable() const override {
        return is_available_;
    }
    bool WriteFile(std::string_view path, const void* data, size_t size, bool append) override {
        return inner_->WriteFile(path, data, size, append);
    }
    bool ReadFile(std::string_view path, void* data, size_t size, size_t& out_len) override {
        return inner_->ReadFile(path, data, size, out_len);
    }
    int OpenFile(std::string_view path, fs_mode_t flags, fs_file_t* file) override {
        return inner_->OpenFile(path, flags, file);
    }
    bool CreateDirectory(std::string_view path) override {
        return inner_->CreateDirectory(path);
    }
    bool Exists(std::string_view path) const override {
        return inner_->Exists(path);
    }
    bool DeleteFile(std::string_view path) override {
        return inner_->DeleteFile(path);
    }
    bool DeleteRecursive(std::string_view path) override {
        return inner_->DeleteRecursive(path);
    }
    int Rename(std::string_view from, std::string_view to) override {
        return inner_->Rename(from, to);
    }
    std::vector<std::string> ListFiles(std::string_view path) const override {
        return inner_->ListFiles(path);
    }
    int ListDirectory(
        std::string_view path,
        size_t first,
        size_t max_entries,
        std::vector<DirectoryEntry>& entries,
        size_t& count
    ) const override {
        return inner_->ListDirectory(path, first, max_entries, entries, count);
    }
    std::optional<size_t> GetFileSize(std::string_view path) const override {
        return inner_->GetFileSize(path);
    }
    uint64_t GetTotalSpace() const override {
        return inner_->GetTotalSpace();
    }
    uint64_t GetUsedSpace() const override {
        return inner_->GetUsedSpace();
    }
    bool Format() override {
        return inner_->Format();
    }
};

struct Rig {
    // The internal file system and, behind sd, the volume that stands in for the SD card.
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<SwitchableVolume> sd;
    std::shared_ptr<WorkQueueThread> work_queue;
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<AssetsManager> assets;
};

// A formatted volume (also behind the SD service, a distinct object) and a configuration work queue, as
// the other cases of the suite.
inline Rig MakeRig(const char* work_queue_name) {
    DtFs::InitInternalFs();

    Rig rig;
    rig.fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    rig.fs_service->Format();
    rig.sd = std::make_shared<SwitchableVolume>(rig.fs_service);

    rig.work_queue = std::make_shared<WorkQueueThread>(work_queue_name, 6144, 5);
    zassert_true(rig.work_queue->Initialize());

    rig.time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );
    rig.frame = std::make_shared<SensorReadingsFrame>();
    rig.assets = std::make_shared<AssetsManager>();

    return rig;
}

// An empty file: a configuration that refers to a script needs it to exist while the card is mounted,
// and a script of no bytes is never loaded.
inline void WriteScript(FsService& fs_service, std::string_view path) {
    const size_t slash = path.rfind('/');
    if(slash != std::string_view::npos)
        zassert_true(fs_service.CreateDirectory(path.substr(0, slash)), "The scripts directory was not created");

    zassert_true(fs_service.WriteFile(path, "", 0, false), "The script was not written");
}

inline std::shared_ptr<Sensor> MakeSensor(std::string_view id, std::string_view script_path, bool is_enabled = true) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = "1 + 1";
    sensor->configuration.sampling_rate_ms = 20;
    sensor->configuration.script_path = script_path;
    sensor->is_enabled = is_enabled;

    return sensor;
}

// The SD volume and the registry are optional, as the controller takes them.
inline std::unique_ptr<SensorsController> MakeSensorsController(
    const Rig& rig,
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<AssetsManager> assets
) {

    return std::make_unique<SensorsController>(
        rig.fs_service, rig.work_queue, nullptr, rig.time_service, rig.frame, nullptr, nullptr, nullptr, nullptr,
        std::move(sd_fs_service), std::move(assets)
    );
}

// Initializes with @p sensors already stored, so that the controller's update handler does not fire.
inline int InitializeWithSensors(SensorsController& controller, std::vector<std::shared_ptr<Sensor>> sensors) {
    return controller.Initialize([&sensors](const auto& manager) {
        zassert_true(manager->Update(sensors), "The sensors were not stored");
    });
}

struct CanMessageSpec {
    uint8_t bus_channel;
    uint32_t frame_id;
    std::string_view script_path;
    bool is_enabled = true;
};

// Stores a configuration with the given messages on classical CAN channels, one channel per bus.
inline void StoreCanConfiguration(
    eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager& manager,
    std::initializer_list<CanMessageSpec> specs
) {

    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());

    for(const auto& spec : specs) {
        auto channel = configuration.channel_configurations.find(spec.bus_channel);
        if(channel == configuration.channel_configurations.end()) {
            CanChannelConfiguration new_channel(std::allocator_arg, Mrm::GetDefaultPmr());
            new_channel.type = eerie_leap::subsys::canbus::CanbusType::CLASSICAL_CAN;
            new_channel.bus_channel = spec.bus_channel;
            new_channel.bitrate = 500000;

            channel = configuration.channel_configurations.emplace(spec.bus_channel, std::move(new_channel)).first;
        }

        auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
        message->frame_id = spec.frame_id;
        message->is_enabled = spec.is_enabled;
        message->name = "EL_FRAME_" + std::to_string(spec.frame_id);
        message->message_size = 8;
        message->send_interval_ms = 10;
        message->script_path = spec.script_path;

        channel->second.message_configurations.push_back(std::move(message));
    }

    zassert_true(manager.Update(configuration), "The CAN configuration was not stored");
}

inline std::unique_ptr<CanbusController>
MakeCanbusController(const Rig& rig, std::shared_ptr<IFsService> sd_fs_service, std::shared_ptr<AssetsManager> assets) {

    return std::make_unique<CanbusController>(
        rig.fs_service, rig.work_queue, nullptr, std::move(sd_fs_service), std::move(assets)
    );
}

// Initializes with @p specs already stored, so that the controller's update handler does not fire.
inline int InitializeWithMessages(CanbusController& controller, std::initializer_list<CanMessageSpec> specs) {
    return controller.Initialize([specs](const auto& manager) {
        StoreCanConfiguration(*manager, specs);
    });
}

} // namespace script_assets_test

#endif // CONFIG_EERIE_LEAP_ASSETS
