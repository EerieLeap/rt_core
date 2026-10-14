#pragma once

#include <array>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "subsys/device_tree/dt_adc.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/adc/i_adc.h"
#include "subsys/adc/adc_emulator.h"
#include "subsys/adc/adc_manager.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"

#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/adc_domain/models/adc_configuration.h"
#include "domain/adc_domain/models/adc_range.h"
#include "domain/adc_domain/utilities/adc_calibrator.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

// Shared by the suites that run analog sensors: a scripted ADC, the emulated one, hand-made ADC
// configurations and the services every processing test builds.
namespace processing_test_support {

using eerie_leap::utilities::memory::Mrm;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::utilities::voltage_interpolator::CalibrationData;
using eerie_leap::utilities::voltage_interpolator::InterpolationMethod;
using eerie_leap::subsys::device_tree::DtAdc;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::adc::AdcCapabilities;
using eerie_leap::subsys::adc::AdcEmulator;
using eerie_leap::subsys::adc::AdcError;
using eerie_leap::subsys::adc::AdcManager;
using eerie_leap::subsys::adc::AdcSampleRequest;
using eerie_leap::subsys::adc::AdcStats;
using eerie_leap::subsys::adc::IAdc;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::domain::adc_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::adc_domain::models::AdcChannelConfiguration;
using eerie_leap::domain::adc_domain::models::AdcConfiguration;
using eerie_leap::domain::adc_domain::models::AdcRange;
using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::sensor_readers::SensorReaderFactory;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

// vref 3300 mV, gain 1, 12 bits on the overlay's first emulated ADC.
constexpr float kEmulLsbVolts = 3.3F / 4096.0F;
// adc_emul truncates mV × 4095 / 3300 to a whole count, so a constant input lands up to ~1.3 LSB low.
constexpr float kEmulTolerance = 2.0F * kEmulLsbVolts;

// Answers every request from a script: channel c reads volts_base + c × volts_step, a channel listed
// in failing_channel reports a driver error. Counts requests; Sample() never allocates.
class FakeAdc : public IAdc {
private:
    int channel_count_;

public:
    static constexpr size_t kMaxChannels = 8;

    float volts_base = 1.0F;
    float volts_step = 0.0F;
    int failing_channel = -1;

    int requests = 0;
    std::array<uint8_t, kMaxChannels> last_channels {};
    size_t last_channel_count = 0;
    uint16_t last_samples = 0;

    explicit FakeAdc(int channel_count) : channel_count_(channel_count) {}

    bool Initialize() override { return true; }

    void Sample(const AdcSampleRequest& request) noexcept override {
        requests++;
        last_samples = request.samples;
        last_channel_count = request.channels.size();

        for(size_t i = 0; i < request.channels.size(); i++) {
            const uint8_t channel = request.channels[i];
            if(i < kMaxChannels)
                last_channels[i] = channel;

            request.results[i] = {};
            if(channel == failing_channel) {
                request.results[i].error = AdcError::DRIVER;
                request.results[i].errno_value = -5;
            } else {
                request.results[i].volts = volts_base + volts_step * channel;
            }
        }
    }

    [[nodiscard]] int GetChannelCount() const override { return channel_count_; }
    [[nodiscard]] AdcCapabilities GetCapabilities() const override { return {}; }
    [[nodiscard]] AdcStats GetStats() const override { return { .requests = static_cast<uint32_t>(requests) }; }
};

inline std::shared_ptr<TimeService> MakeTimeService() {
    return std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
}

inline std::shared_ptr<FsService> MakeFormattedFsService() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    return fs_service;
}

// The overlay's first emulated ADC, every channel back on a constant so a value function a
// previous test installed is never called.
inline std::shared_ptr<AdcEmulator> MakeEmulator() {
    DtAdc::Initialize();

    const auto& info = DtAdc::Get().value().at(0);
    for(const auto& channel_config : info.channel_configs)
        zassert_ok(adc_emul_const_value_set(info.adc_device, channel_config.channel_id, 0));

    return std::make_shared<AdcEmulator>(info);
}

inline std::shared_ptr<AdcManager> MakeAdcManager(std::shared_ptr<IAdc> adc) {
    auto adc_manager = std::make_shared<AdcManager>(std::vector<std::shared_ptr<IAdc>> { std::move(adc) });
    zassert_true(adc_manager->Initialize());

    return adc_manager;
}

// Pins an emulator channel to a constant input instead of the random stimulus.
inline void DriveConstant(AdcEmulator& adc, int channel, uint32_t mv) {
    const auto& info = DtAdc::Get().value().at(0);

    adc.SetExternallyDriven(channel);
    zassert_ok(adc_emul_const_value_set(info.adc_device, info.channel_configs.at(channel).channel_id, mv));
}

inline std::shared_ptr<AdcCalibrator> MakeCalibrator(
    std::initializer_list<CalibrationData> table, AdcRange range = AdcRange::Default()) {

    auto calibration_table = std::make_shared<std::pmr::vector<CalibrationData>>(table, Mrm::GetDefaultPmr());

    return std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, std::move(calibration_table), range);
}

// One calibrator shared by every channel, as a hand-made configuration rather than one the
// manager padded.
inline std::shared_ptr<AdcConfiguration> MakeAdcConfiguration(
    size_t channel_count, uint16_t samples, std::shared_ptr<AdcCalibrator> calibrator) {

    auto channel_configurations = std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>();
    for(size_t i = 0; i < channel_count; i++)
        channel_configurations->push_back(std::make_shared<AdcChannelConfiguration>(calibrator));

    auto adc_configuration = std::make_shared<AdcConfiguration>();
    adc_configuration->samples = samples;
    adc_configuration->channel_configurations = std::move(channel_configurations);

    return adc_configuration;
}

// Reads 0..5 V as 0..100.
inline std::shared_ptr<Sensor> MakeAnalogSensor(std::string_view id, int channel, int sampling_rate_ms) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::PHYSICAL_ANALOG;
    sensor->configuration.channel = channel;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;
    sensor->configuration.interpolation_method = InterpolationMethod::LINEAR;
    sensor->configuration.calibration_table = { {0.0F, 0.0F}, {5.0F, 100.0F} };

    return sensor;
}

inline std::shared_ptr<Sensor> MakeVirtualSensor(std::string_view id, const char* expression, int sampling_rate_ms) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;

    return sensor;
}

inline std::shared_ptr<AdcConfigurationManager> MakeAdcConfigurationManager(
    std::shared_ptr<FsService> fs_service, std::shared_ptr<IAdcManager> adc_manager) {

    return std::make_shared<AdcConfigurationManager>(
        std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", std::move(fs_service)),
        std::move(adc_manager));
}

// A processing service wired the way SensorsController wires it, without CAN, over a stored
// sensors configuration. Initialized, not started.
struct ProcessingRig {
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager;
    std::shared_ptr<SensorsProcessingService> processing_service;
};

// The sensors configuration is validated against @p adc_channel_count rather than the ADC, so a
// unit without one (null adc_manager and adc_configuration_manager) can still be given analog
// sensors. GPIO sensors get readers, on edges or polled, only when @p gpio is given.
inline ProcessingRig MakeProcessingRig(
    std::shared_ptr<FsService> fs_service,
    std::shared_ptr<IAdcManager> adc_manager,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager,
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    int adc_channel_count,
    std::shared_ptr<IGpio> gpio = nullptr) {

    ProcessingRig rig;
    rig.time_service = MakeTimeService();
    rig.frame = std::make_shared<SensorReadingsFrame>();

    rig.sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", std::move(fs_service)),
        nullptr,
        gpio != nullptr ? gpio->GetChannelCount() : 0,
        adc_channel_count);
    zassert_true(rig.sensors_configuration_manager->Update(sensors), "The sensors configuration is valid");

    rig.processing_service = std::make_shared<SensorsProcessingService>(
        rig.sensors_configuration_manager,
        rig.frame,
        rig.time_service,
        gpio != nullptr ? std::make_shared<IsrSensorReaderFactory>(rig.time_service, gpio) : nullptr,
        std::make_shared<SensorReaderFactory>(rig.time_service, gpio, std::move(adc_manager)),
        std::move(adc_configuration_manager));
    zassert_true(rig.processing_service->Initialize());

    return rig;
}

} // namespace processing_test_support
