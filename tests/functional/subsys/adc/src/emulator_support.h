#pragma once

#include <memory>
#include <zephyr/ztest.h>
#include <zephyr/drivers/adc/adc_emul.h>

#include "subsys/device_tree/dt_adc.h"
#include "subsys/adc/adc_emulator.h"

namespace emulator_support {

using eerie_leap::subsys::device_tree::DtAdc;
using eerie_leap::subsys::adc::AdcEmulator;

// The overlay declares three emulated ADCs: four channels, two channels, then one whose channel
// states no resolution and therefore never initializes.
constexpr int kFirstAdcChannels = 4;
constexpr int kSecondAdcChannels = 2;
constexpr int kDtAdcCount = 3;
constexpr int kDtChannelCount = kFirstAdcChannels + kSecondAdcChannels + 1;
constexpr int kWorkingChannelCount = kFirstAdcChannels + kSecondAdcChannels;

// vref 3300 mV, gain 1, 12 bits.
constexpr float kLsbVolts = 3.3F / 4096.0F;
// adc_emul truncates mV × 4095 / 3300 to a whole count, so a constant input lands up to ~1.3 LSB low.
constexpr float kEmulTolerance = 2.0F * kLsbVolts;

inline const device* Dev(int index) {
    return DtAdc::Get().value().at(index).adc_device;
}

// The emulator keeps value functions across tests, so every channel goes back to a constant
// before a probe could call a finished test's function with a pointer into its dead stack frame.
inline void ResetEmulator() {
    DtAdc::Initialize();

    for(const auto& info : DtAdc::Get().value())
        for(const auto& channel_config : info.channel_configs)
            zassert_ok(adc_emul_const_value_set(info.adc_device, channel_config.channel_id, 0));
}

inline std::shared_ptr<AdcEmulator> MakeEmulator(int index, bool initialize = true) {
    ResetEmulator();

    auto adc = std::make_shared<AdcEmulator>(DtAdc::Get().value().at(index));
    if(initialize)
        zassert_true(adc->Initialize(), "ADC %d failed to initialize", index);

    return adc;
}

// Pins a channel to a constant that the emulator keeps instead of the random stimulus.
inline void DriveConstant(AdcEmulator& adc, int index, int channel, uint32_t mv) {
    adc.SetExternallyDriven(channel);
    zassert_ok(adc_emul_const_value_set(Dev(index), channel, mv));
}

struct CountedFailure {
    int failures_left;
    int calls = 0;
    int err;
    uint32_t mv;
};

inline int FailThenSucceed(const device*, unsigned int, void* data, uint32_t* result) {
    auto* state = static_cast<CountedFailure*>(data);
    state->calls++;

    if(state->failures_left > 0) {
        state->failures_left--;
        return state->err;
    }

    *result = state->mv;
    return 0;
}

struct CountedSuccess {
    int successes_left;
    int calls = 0;
    int err;
    uint32_t mv;
};

// Converts successes_left times, then fails every call.
inline int SucceedThenFail(const device*, unsigned int, void* data, uint32_t* result) {
    auto* state = static_cast<CountedSuccess*>(data);
    state->calls++;

    if(state->successes_left <= 0)
        return state->err;

    state->successes_left--;
    *result = state->mv;
    return 0;
}

inline int AlternatingRaw(const device*, unsigned int, void* data, uint32_t* result) {
    auto* calls = static_cast<int*>(data);
    *result = 100 + ((*calls)++ & 1);
    return 0;
}

inline int CountingConstant(const device*, unsigned int, void* data, uint32_t* result) {
    auto* calls = static_cast<int*>(data);
    (*calls)++;
    *result = 1000;
    return 0;
}

} // namespace emulator_support
