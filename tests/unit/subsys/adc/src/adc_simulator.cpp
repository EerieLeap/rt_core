#include <memory>
#include <stdexcept>
#include <vector>
#include <zephyr/ztest.h>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/adc/models/adc_configuration.h"

using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::models;
using eerie_leap::subsys::adc::utilities::AdcCalibrator;

namespace {

std::shared_ptr<AdcConfiguration> MakeConfiguration(int channels, uint16_t samples = 40) {
    auto calibration_table = std::make_shared<std::pmr::vector<CalibrationData>>(
        std::pmr::vector<CalibrationData> {{0.0F, 0.0F}, {3.3F, 3.3F}});
    auto calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, calibration_table);

    auto channel_configurations = std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>();
    for(int i = 0; i < channels; ++i)
        channel_configurations->push_back(std::make_shared<AdcChannelConfiguration>(calibrator));

    auto configuration = std::make_shared<AdcConfiguration>();
    configuration->samples = samples;
    configuration->channel_configurations = channel_configurations;

    return configuration;
}

} // namespace

ZTEST_SUITE(adc_simulator, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_simulator, test_Initialize_succeeds) {
    AdcSimulator adc;

    zassert_true(adc.Initialize());
    zassert_equal(adc.GetChannelCount(), 8);
}

ZTEST(adc_simulator, test_ReadChannel_requires_configuration) {
    AdcSimulator adc;
    adc.Initialize();

    bool threw = false;
    try {
        adc.ReadChannel(0);
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "reading without a sample count should be rejected");
}

ZTEST(adc_simulator, test_ReadChannel_rejects_out_of_range_channels) {
    AdcSimulator adc;
    adc.Initialize();
    adc.UpdateConfiguration(16);

    for(int channel : { -1, 8, 100 }) {
        bool threw = false;
        try {
            adc.ReadChannel(channel);
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "channel %d should be rejected", channel);
    }
}

ZTEST(adc_simulator, test_ReadChannel_stays_within_the_adc_range) {
    AdcSimulator adc;
    adc.Initialize();
    adc.UpdateConfiguration(16);

    for(int channel = 0; channel < adc.GetChannelCount(); ++channel)
        for(int i = 0; i < 50; ++i)
            zassert_between_inclusive(adc.ReadChannel(channel), 0.0, 3.3);
}

ZTEST(adc_simulator, test_ReadChannel_varies) {
    AdcSimulator adc;
    adc.Initialize();
    adc.UpdateConfiguration(16);

    float first = adc.ReadChannel(0);
    bool changed = false;
    for(int i = 0; i < 20 && !changed; ++i)
        changed = adc.ReadChannel(0) != first;

    zassert_true(changed, "simulated readings should not be constant");
}

ZTEST_SUITE(adc_simulator_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_simulator_manager, test_counts) {
    AdcSimulatorManager manager;

    zassert_true(manager.Initialize());
    zassert_equal(manager.GetAdcCount(), 1);
    zassert_equal(manager.GetChannelCount(), 8);
}

ZTEST(adc_simulator_manager, test_UpdateConfiguration_rejects_null) {
    AdcSimulatorManager manager;

    bool threw = false;
    try {
        manager.UpdateConfiguration(nullptr);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_simulator_manager, test_GetChannelReader_requires_configuration) {
    AdcSimulatorManager manager;
    manager.Initialize();

    bool threw = false;
    try {
        manager.GetChannelReader(0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_simulator_manager, test_GetChannelConfiguration_requires_configuration) {
    AdcSimulatorManager manager;
    manager.Initialize();

    bool threw = false;
    try {
        manager.GetChannelConfiguration(0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_simulator_manager, test_ResetSamplesCount_requires_configuration) {
    AdcSimulatorManager manager;
    manager.Initialize();

    bool threw = false;
    try {
        manager.ResetSamplesCount();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_simulator_manager, test_GetChannelReader_reads_within_range) {
    AdcSimulatorManager manager;
    manager.UpdateConfiguration(MakeConfiguration(4));
    manager.Initialize();

    for(int channel = 0; channel < 4; ++channel) {
        auto reader = manager.GetChannelReader(channel);

        for(int i = 0; i < 25; ++i)
            zassert_between_inclusive(reader(), 0.0, 3.3);
    }
}

ZTEST(adc_simulator_manager, test_channel_validity_is_bounded_by_the_configuration) {
    AdcSimulatorManager manager;
    manager.UpdateConfiguration(MakeConfiguration(4));
    manager.Initialize();

    // The simulator exposes 8 channels but only 4 are configured.
    for(int channel : { -1, 4, 8 }) {
        bool reader_threw = false;
        bool configuration_threw = false;

        try {
            manager.GetChannelReader(channel);
        } catch(const std::invalid_argument&) {
            reader_threw = true;
        }

        try {
            manager.GetChannelConfiguration(channel);
        } catch(const std::invalid_argument&) {
            configuration_threw = true;
        }

        zassert_true(reader_threw, "GetChannelReader accepted channel %d", channel);
        zassert_true(configuration_threw, "GetChannelConfiguration accepted channel %d", channel);
    }
}

ZTEST(adc_simulator_manager, test_GetChannelConfiguration_returns_configured_channel) {
    auto configuration = MakeConfiguration(4);

    AdcSimulatorManager manager;
    manager.UpdateConfiguration(configuration);
    manager.Initialize();

    for(int channel = 0; channel < 4; ++channel)
        zassert_equal(manager.GetChannelConfiguration(channel).get(),
            configuration->channel_configurations->at(channel).get());
}

ZTEST(adc_simulator_manager, test_sample_count_can_be_overridden_and_restored) {
    AdcSimulatorManager manager;
    manager.UpdateConfiguration(MakeConfiguration(4));
    manager.Initialize();

    manager.UpdateSamplesCount(1);
    zassert_between_inclusive(manager.GetChannelReader(0)(), 0.0, 3.3);

    manager.ResetSamplesCount();
    zassert_between_inclusive(manager.GetChannelReader(0)(), 0.0, 3.3);
}

ZTEST(adc_simulator_manager, test_zero_samples_disables_reading) {
    AdcSimulatorManager manager;
    manager.UpdateConfiguration(MakeConfiguration(4, 0));
    manager.Initialize();

    auto reader = manager.GetChannelReader(0);

    bool threw = false;
    try {
        reader();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "a zero sample count must not produce a reading");
}
