#include <cerrno>
#include <memory>
#include <optional>
#include <unordered_set>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/adc/adc_emulator.h"
#include "subsys/device_tree/dt_adc.h"

#include "emulator_support.h"

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace emulator_support;

namespace {

constexpr AdcCapabilities kNoCapabilities{ .burst = false, .multi_channel = false, .us_per_conversion = 0 };
constexpr AdcCapabilities kBurstOnly{ .burst = true, .multi_channel = false, .us_per_conversion = 0 };

// The three ways Adc::Sample() reaches the driver; nullopt keeps what the probe found (burst + multi-channel).
struct SamplingPath {
    const char* name;
    std::optional<AdcCapabilities> capabilities;
};

constexpr SamplingPath kSamplingPaths[] = { { .name = "multi-channel scan", .capabilities = std::nullopt },
    { .name = "single-channel bursts", .capabilities = kBurstOnly },
    { .name = "per-sample loop", .capabilities = kNoCapabilities } };

std::shared_ptr<AdcEmulator> MakeEmulatorOn(const SamplingPath& path) {
    auto adc = MakeEmulator(0);
    if(path.capabilities.has_value())
        adc->OverrideCapabilities(*path.capabilities);

    return adc;
}

constexpr uint32_t kSlowConversionUs = 50;

// k_busy_wait() advances native_sim's simulated clock too, so the probe measures it everywhere.
int SlowConstant(const device*, unsigned int, void*, uint32_t* result) {
    k_busy_wait(kSlowConversionUs);
    *result = 1000;
    return 0;
}

// The first conversion stalls and times out; every later one is immediate.
int SlowTimeoutThenConstant(const device*, unsigned int, void* data, uint32_t* result) {
    auto* calls = static_cast<int*>(data);
    if((*calls)++ == 0) {
        k_busy_wait(10 * kSlowConversionUs);
        return -ETIMEDOUT;
    }

    *result = 1000;
    return 0;
}

} // namespace

ZTEST_SUITE(adc, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc, test_Initialize_probes_the_driver) {
    auto adc = MakeEmulator(0);

    zassert_equal(adc->GetChannelCount(), kFirstAdcChannels);
    zassert_equal(adc->GetStats().driver_calls, 3, "one timed read, one burst probe, one two-channel probe");
    zassert_equal(adc->GetStats().requests, 0);
    zassert_equal(adc->GetStats().errors, 0);
}

ZTEST(adc, test_capabilities_are_reported) {
    auto adc = MakeEmulator(0);
    auto capabilities = adc->GetCapabilities();

    zassert_true(capabilities.burst, "adc_emul accepts extra_samplings");
    zassert_true(capabilities.multi_channel, "adc_emul accepts a multi-channel mask");

    auto single_channel = MakeEmulator(2, false);
    zassert_equal(single_channel->GetChannelCount(), 1);
}

ZTEST(adc, test_probe_times_its_first_conversion) {
    auto adc = MakeEmulator(0, false);
    adc->SetExternallyDriven(0);
    zassert_ok(adc_emul_value_func_set(Dev(0), 0, SlowConstant, nullptr));

    zassert_true(adc->Initialize());
    zassert_true(
        adc->GetCapabilities().us_per_conversion >= kSlowConversionUs, "measured %u us",
        adc->GetCapabilities().us_per_conversion
    );
}

ZTEST(adc, test_probe_times_only_the_successful_attempt) {
    auto adc = MakeEmulator(0, false);

    int calls = 0;
    adc->SetExternallyDriven(0);
    zassert_ok(adc_emul_value_func_set(Dev(0), 0, SlowTimeoutThenConstant, &calls));

    zassert_true(adc->Initialize(), "a timeout of the first attempt is retried");
    zassert_equal(adc->GetStats().timeouts, 1);
    zassert_true(
        adc->GetCapabilities().us_per_conversion < 10 * kSlowConversionUs,
        "the timed-out attempt is not part of the %u us", adc->GetCapabilities().us_per_conversion
    );
}

ZTEST(adc, test_Initialize_rejects_duplicate_channel_ids) {
    ResetEmulator();

    auto info = DtAdc::Get().value().at(0);
    info.channel_configs[1].channel_id = info.channel_configs[0].channel_id;

    AdcEmulator adc(info);
    zassert_false(adc.Initialize(), "two channels on one id would leave one of them unconverted");
}

ZTEST(adc, test_Initialize_rejects_differential_channels) {
    ResetEmulator();

    auto info = DtAdc::Get().value().at(0);
    info.channel_configs[0].differential = 1;

    AdcEmulator adc(info);
    zassert_false(adc.Initialize(), "signed samples cannot be decoded");
}

ZTEST(adc, test_Initialize_takes_the_reference_from_the_driver) {
    ResetEmulator();

    // As if the overlay stated no zephyr,vref-mv: adc_emul knows ref-external1-mv = 5000.
    auto info = DtAdc::Get().value().at(1);
    info.channel_configs[0].reference = ADC_REF_EXTERNAL1;
    info.references_mv[0] = 0;

    AdcEmulator adc(info);
    zassert_true(adc.Initialize());
    DriveConstant(adc, 1, 0, 2500);

    const uint8_t channels[1] = { 0 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 10 };
    adc.Sample(request);

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_within(results[0].volts, 2.5F, 2.0F * 5.0F / 4096.0F, "read %f V", static_cast<double>(results[0].volts));
}

ZTEST(adc, test_Initialize_rejects_resolution_zero) {
    ResetEmulator();

    auto adcs = DtAdc::Get().value();
    zassert_equal(adcs.size(), kDtAdcCount);
    zassert_equal(adcs[2].resolutions.at(0), 0, "the third device's channel states no resolution");

    AdcEmulator first(adcs[0]);
    AdcEmulator second(adcs[1]);
    AdcEmulator third(adcs[2]);

    zassert_true(first.Initialize());
    zassert_true(second.Initialize());
    zassert_false(third.Initialize(), "a channel without a resolution cannot be converted");
}

ZTEST(adc, test_Sample_reads_every_requested_channel) {
    auto adc = MakeEmulator(0);
    for(int channel = 0; channel < kFirstAdcChannels; channel++)
        DriveConstant(*adc, 0, channel, 500 * (channel + 1));

    const uint8_t channels[kFirstAdcChannels] = { 0, 1, 2, 3 };
    AdcSample results[kFirstAdcChannels];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 100 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    for(int channel = 0; channel < kFirstAdcChannels; channel++) {
        zassert_equal(results[channel].error, AdcError::NONE, "channel %d", channel);
        zassert_equal(results[channel].errno_value, 0);
        zassert_within(
            results[channel].volts, 0.5F * (channel + 1), kEmulTolerance, "channel %d read %f V", channel,
            static_cast<double>(results[channel].volts)
        );
    }

    zassert_equal(after.requests - before.requests, 1);
    zassert_equal(after.conversions - before.conversions, 100 * kFirstAdcChannels);
    zassert_equal(after.errors, 0);
}

ZTEST(adc, test_Sample_reads_a_subset_in_request_order) {
    auto adc = MakeEmulator(0);
    DriveConstant(*adc, 0, 1, 1000);
    DriveConstant(*adc, 0, 3, 3000);

    const uint8_t channels[2] = { 1, 3 };
    AdcSample results[2];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 10 };
    adc->Sample(request);

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_equal(results[1].error, AdcError::NONE);
    zassert_within(results[0].volts, 1.0F, kEmulTolerance);
    zassert_within(results[1].volts, 3.0F, kEmulTolerance);
}

ZTEST(adc, test_Sample_reaches_the_second_adc) {
    auto adc = MakeEmulator(1);
    DriveConstant(*adc, 1, 1, 2200);

    const uint8_t channels[1] = { 1 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 1 };
    adc->Sample(request);

    zassert_equal(adc->GetChannelCount(), kSecondAdcChannels);
    zassert_equal(results[0].error, AdcError::NONE);
    zassert_within(results[0].volts, 2.2F, kEmulTolerance);
}

ZTEST(adc, test_Sample_varies_with_the_random_stimulus) {
    auto adc = MakeEmulator(0);

    const uint8_t channels[1] = { 2 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 10 };

    std::unordered_set<float> readings;
    for(int i = 0; i < 100; i++) {
        adc->Sample(request);

        zassert_equal(results[0].error, AdcError::NONE);
        zassert_between_inclusive(results[0].volts, 0.0F, 3.3F);
        readings.insert(results[0].volts);
    }

    // The emulator quantises to whole millivolts, so a handful of identical means
    // is expected; what matters is that consecutive requests keep changing.
    zassert_true(readings.size() >= 90, "expected varying readings, got %zu distinct values", readings.size());
}

ZTEST(adc, test_Sample_reports_a_driver_error_per_channel) {
    constexpr uint16_t kSamples = 40;

    for(const auto& path : kSamplingPaths) {
        auto adc = MakeEmulatorOn(path);
        DriveConstant(*adc, 0, 0, 1000);
        DriveConstant(*adc, 0, 2, 2000);

        CountedFailure always{ .failures_left = INT32_MAX, .err = -EIO, .mv = 0 };
        adc->SetExternallyDriven(1);
        zassert_ok(adc_emul_value_func_set(Dev(0), 1, FailThenSucceed, &always));

        const uint8_t channels[3] = { 0, 1, 2 };
        AdcSample results[3];
        AdcSampleRequest request{ .channels = channels, .results = results, .samples = kSamples };

        auto before = adc->GetStats();
        adc->Sample(request);
        auto after = adc->GetStats();

        zassert_equal(results[0].error, AdcError::NONE, "%s", path.name);
        zassert_within(results[0].volts, 1.0F, kEmulTolerance, "%s", path.name);
        zassert_equal(results[1].error, AdcError::DRIVER, "%s", path.name);
        zassert_equal(results[1].errno_value, -EIO, "%s", path.name);
        zassert_equal(results[2].error, AdcError::NONE, "%s", path.name);
        zassert_within(results[2].volts, 2.0F, kEmulTolerance, "%s", path.name);

        zassert_equal(after.errors - before.errors, 1, "%s", path.name);
        zassert_equal(after.last_errno, -EIO, "%s", path.name);
        zassert_equal(after.timeouts, before.timeouts, "%s: -EIO is not a timeout", path.name);
        zassert_equal(
            after.conversions - before.conversions, 2 * kSamples,
            "%s: only the valid channels' conversions are counted", path.name
        );
    }
}

ZTEST(adc, test_Sample_resumes_a_failed_scan_per_channel) {
    constexpr uint16_t kChunk = CONFIG_EERIE_LEAP_ADC_BURST_CHUNK;
    constexpr uint16_t kSamples = 100;
    static_assert(kSamples > kChunk && kSamples <= 2 * kChunk, "the scan fails in its second chunk");

    auto adc = MakeEmulator(0);
    zassert_true(adc->GetCapabilities().burst && adc->GetCapabilities().multi_channel);
    DriveConstant(*adc, 0, 0, 1000);
    DriveConstant(*adc, 0, 2, 2000);

    // Channel 1 converts the whole first chunk, then fails.
    CountedSuccess first_chunk{ .successes_left = kChunk, .err = -EIO, .mv = 1500 };
    adc->SetExternallyDriven(1);
    zassert_ok(adc_emul_value_func_set(Dev(0), 1, SucceedThenFail, &first_chunk));

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[3];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = kSamples };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_within(results[0].volts, 1.0F, kEmulTolerance, "read %f V", static_cast<double>(results[0].volts));
    zassert_equal(results[1].error, AdcError::DRIVER);
    zassert_equal(results[1].errno_value, -EIO);
    zassert_equal(results[2].error, AdcError::NONE);
    zassert_within(results[2].volts, 2.0F, kEmulTolerance, "read %f V", static_cast<double>(results[2].volts));

    zassert_equal(
        after.driver_calls - before.driver_calls, 5,
        "two scan chunks, then one burst per channel for the outstanding samplings"
    );
    zassert_equal(
        after.conversions - before.conversions, 3 * kChunk + 2 * (kSamples - kChunk),
        "the completed chunk is kept, not sampled again"
    );
    zassert_equal(after.errors - before.errors, 1);
}

ZTEST(adc, test_Sample_times_out_every_channel_of_a_scan) {
    auto adc = MakeEmulator(0);
    zassert_true(adc->GetCapabilities().burst && adc->GetCapabilities().multi_channel);

    CountedFailure always{ .failures_left = INT32_MAX, .err = -ETIMEDOUT, .mv = 0 };
    adc->SetExternallyDriven(0);
    zassert_ok(adc_emul_value_func_set(Dev(0), 0, FailThenSucceed, &always));

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[3];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 40 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    for(int i = 0; i < 3; i++) {
        zassert_equal(results[i].error, AdcError::TIMEOUT, "channel %d", i);
        zassert_equal(results[i].errno_value, -ETIMEDOUT, "channel %d", i);
    }

    zassert_equal(always.calls, 3);
    zassert_equal(after.driver_calls - before.driver_calls, 3, "the scan's retries only, no per-channel fallback");
    zassert_equal(after.timeouts - before.timeouts, 3);
    zassert_equal(after.errors - before.errors, 3);
    zassert_equal(after.conversions, before.conversions);
}

ZTEST(adc, test_Sample_retries_timeouts_then_fails) {
    for(const auto& path : kSamplingPaths) {
        auto adc = MakeEmulatorOn(path);
        // The four good conversions take one burst, or one call each on the loop.
        const uint32_t good_calls = adc->GetCapabilities().burst ? 1 : 4;

        CountedFailure twice{ .failures_left = 2, .err = -ETIMEDOUT, .mv = 1650 };
        adc->SetExternallyDriven(0);
        zassert_ok(adc_emul_value_func_set(Dev(0), 0, FailThenSucceed, &twice));

        const uint8_t channels[1] = { 0 };
        AdcSample results[1];
        AdcSampleRequest request{ .channels = channels, .results = results, .samples = 4 };

        auto before = adc->GetStats();
        adc->Sample(request);
        auto after = adc->GetStats();

        zassert_equal(results[0].error, AdcError::NONE, "%s: two timeouts are retried", path.name);
        zassert_within(results[0].volts, 1.65F, kEmulTolerance, "%s", path.name);
        zassert_equal(twice.calls, 6, "%s: two failed and four good conversions", path.name);
        zassert_equal(after.timeouts - before.timeouts, 2, "%s", path.name);
        zassert_equal(after.driver_calls - before.driver_calls, 2 + good_calls, "%s", path.name);
        zassert_equal(after.errors - before.errors, 0, "%s", path.name);

        CountedFailure thrice{ .failures_left = 3, .err = -ETIMEDOUT, .mv = 1650 };
        zassert_ok(adc_emul_value_func_set(Dev(0), 0, FailThenSucceed, &thrice));

        before = adc->GetStats();
        adc->Sample(request);
        after = adc->GetStats();

        zassert_equal(results[0].error, AdcError::TIMEOUT, "%s: a third timeout fails the channel", path.name);
        zassert_equal(results[0].errno_value, -ETIMEDOUT, "%s", path.name);
        zassert_equal(thrice.calls, 3, "%s: sampling of the channel stops at the third timeout", path.name);
        zassert_equal(after.driver_calls - before.driver_calls, 3, "%s", path.name);
        zassert_equal(after.timeouts - before.timeouts, 3, "%s", path.name);
        zassert_equal(after.errors - before.errors, 1, "%s", path.name);
        zassert_equal(after.last_errno, -ETIMEDOUT, "%s", path.name);
    }
}

ZTEST(adc, test_Sample_discards_a_partial_burst) {
    constexpr uint16_t kSamples = 64;
    static_assert(CONFIG_EERIE_LEAP_ADC_BURST_CHUNK >= kSamples, "one burst covers the request");

    auto adc = MakeEmulator(0);
    adc->OverrideCapabilities(kBurstOnly);

    // The tenth conversion of the burst fails after nine have reached the buffer.
    CountedSuccess nine{ .successes_left = 9, .err = -EIO, .mv = 1000 };
    adc->SetExternallyDriven(2);
    zassert_ok(adc_emul_value_func_set(Dev(0), 2, SucceedThenFail, &nine));

    const uint8_t channels[1] = { 2 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = kSamples };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::DRIVER);
    zassert_equal(results[0].errno_value, -EIO);
    zassert_equal(nine.calls, 10);
    zassert_equal(after.driver_calls - before.driver_calls, 1);
    zassert_equal(after.conversions, before.conversions, "nothing of the failed burst reaches an accumulator");
}

ZTEST(adc, test_Sample_keeps_fractional_lsb) {
    auto adc = MakeEmulator(0);

    int calls = 0;
    adc->SetExternallyDriven(2);
    zassert_ok(adc_emul_raw_value_func_set(Dev(0), 2, AlternatingRaw, &calls));

    const uint8_t channels[1] = { 2 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 100 };
    adc->Sample(request);

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_equal(calls, 100);
    zassert_within(
        results[0].volts, 100.5F * kLsbVolts, 0.1F * kLsbVolts, "mean of 100/101 LSB read as %f V",
        static_cast<double>(results[0].volts)
    );
}

ZTEST(adc, test_Sample_uses_one_driver_call_per_chunk) {
    auto adc = MakeEmulator(0);
    zassert_true(adc->GetCapabilities().burst && adc->GetCapabilities().multi_channel);

    int calls_0 = 0;
    int calls_1 = 0;
    adc->SetExternallyDriven(0);
    adc->SetExternallyDriven(1);
    zassert_ok(adc_emul_value_func_set(Dev(0), 0, CountingConstant, &calls_0));
    zassert_ok(adc_emul_value_func_set(Dev(0), 1, CountingConstant, &calls_1));

    constexpr uint16_t kSamples = 100;
    constexpr uint32_t kChunks = (kSamples + CONFIG_EERIE_LEAP_ADC_BURST_CHUNK - 1) / CONFIG_EERIE_LEAP_ADC_BURST_CHUNK;
    static_assert(kChunks == 2, "the test counts chunks of 64");

    const uint8_t channels[2] = { 0, 1 };
    AdcSample results[2];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = kSamples };

    auto expect_sampled = [&](uint32_t driver_calls, const char* path) {
        calls_0 = 0;
        calls_1 = 0;

        auto before = adc->GetStats();
        adc->Sample(request);
        auto after = adc->GetStats();

        zassert_equal(after.driver_calls - before.driver_calls, driver_calls, "%s: driver calls", path);
        zassert_equal(after.conversions - before.conversions, 2 * kSamples, "%s: conversions", path);
        zassert_equal(calls_0, kSamples, "%s: channel 0 conversions", path);
        zassert_equal(calls_1, kSamples, "%s: channel 1 conversions", path);
        zassert_equal(results[0].error, AdcError::NONE);
        zassert_equal(results[1].error, AdcError::NONE);
        zassert_within(results[0].volts, 1.0F, kEmulTolerance);
        zassert_within(results[1].volts, 1.0F, kEmulTolerance);
    };

    expect_sampled(kChunks, "multi-channel scan");

    adc->OverrideCapabilities(kBurstOnly);
    expect_sampled(2 * kChunks, "single-channel bursts");

    adc->OverrideCapabilities(kNoCapabilities);
    expect_sampled(2 * kSamples, "per-sample loop");
}

ZTEST(adc, test_Sample_clamps_samples_to_at_least_one) {
    auto adc = MakeEmulator(0);
    DriveConstant(*adc, 0, 0, 1000);

    const uint8_t channels[1] = { 0 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 0 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::NONE, "zero samples must not divide by zero");
    zassert_within(results[0].volts, 1.0F, kEmulTolerance);
    zassert_equal(after.conversions - before.conversions, 1);
}

ZTEST(adc, test_Sample_clamps_samples_to_the_configured_maximum) {
    auto adc = MakeEmulator(0);
    DriveConstant(*adc, 0, 0, 1000);

    const uint8_t channels[1] = { 0 };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = UINT16_MAX };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_within(results[0].volts, 1.0F, kEmulTolerance);
    zassert_equal(after.conversions - before.conversions, CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX);
}

ZTEST(adc, test_Sample_accepts_an_empty_request) {
    auto adc = MakeEmulator(0);

    auto before = adc->GetStats();
    adc->Sample(AdcSampleRequest{});
    auto after = adc->GetStats();

    zassert_equal(after.requests - before.requests, 1);
    zassert_equal(after.driver_calls, before.driver_calls);
    zassert_equal(after.errors, before.errors);
}

ZTEST(adc, test_Sample_marks_out_of_range_and_unordered_channels) {
    auto adc = MakeEmulator(0);

    const uint8_t channels[4] = { 1, kFirstAdcChannels, 100, 1 };
    AdcSample results[4];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 4 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::NONE, "a valid channel is still read");
    zassert_between_inclusive(results[0].volts, 0.0F, 3.3F);

    for(int i : { 1, 2 }) {
        zassert_equal(results[i].error, AdcError::CHANNEL_OUT_OF_RANGE, "channel %u", channels[i]);
        zassert_equal(results[i].errno_value, -EINVAL);
    }

    zassert_equal(results[3].error, AdcError::CHANNEL_OUT_OF_RANGE, "channels must be ascending");
    zassert_equal(after.errors - before.errors, 3);
    zassert_equal(after.conversions - before.conversions, 4, "only the valid channel is converted");
}

ZTEST(adc, test_Sample_before_Initialize_fails_every_channel) {
    auto adc = MakeEmulator(0, false);

    const uint8_t channels[2] = { 0, 1 };
    AdcSample results[2];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 1 };
    adc->Sample(request);

    for(int i = 0; i < 2; i++) {
        zassert_equal(results[i].error, AdcError::DRIVER);
        zassert_equal(results[i].errno_value, -ENODEV);
    }

    zassert_equal(adc->GetStats().driver_calls, 0, "nothing reaches the driver");
}

ZTEST(adc, test_Sample_after_a_failed_probe_fails_every_channel) {
    auto adc = MakeEmulator(0, false);

    CountedFailure always{ .failures_left = INT32_MAX, .err = -EIO, .mv = 0 };
    adc->SetExternallyDriven(0);
    zassert_ok(adc_emul_value_func_set(Dev(0), 0, FailThenSucceed, &always));
    zassert_false(adc->Initialize(), "the probe conversion fails");

    const uint8_t channels[2] = { 0, 1 };
    AdcSample results[2];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 1 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    for(int i = 0; i < 2; i++) {
        zassert_equal(results[i].error, AdcError::DRIVER);
        zassert_equal(results[i].errno_value, -ENODEV);
    }

    zassert_equal(after.driver_calls, before.driver_calls, "nothing reaches the driver");
}

ZTEST(adc, test_Sample_handles_fewer_results_than_channels) {
    auto adc = MakeEmulator(0);

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[2];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = 2 };

    auto before = adc->GetStats();
    adc->Sample(request);
    auto after = adc->GetStats();

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_equal(results[1].error, AdcError::NONE);
    zassert_equal(after.conversions - before.conversions, 4, "only the channels with a result slot are read");
}
