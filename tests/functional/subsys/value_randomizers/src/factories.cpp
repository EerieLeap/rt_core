#include <memory>
#include <optional>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/adc/adc.h"
#include "subsys/adc/adc_factory.hpp"
#include "subsys/adc/adc_randomizer.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/gpio/composite_gpio.h"
#include "subsys/gpio/gpio.h"
#include "subsys/gpio/gpio_factory.hpp"
#include "subsys/gpio/gpio_randomizer.h"
#include "subsys/gpio/gpio_simulator.h"

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::gpio;
using eerie_leap::subsys::device_tree::AdcDTInfo;

// prj.conf: 3 random ADC channels, 5 random GPIO channels with 10-30 ms holds.
namespace {

constexpr int RANDOM_ADC_CHANNELS = 3;
constexpr int RANDOM_GPIO_CHANNELS = 5;

std::optional<std::vector<AdcDTInfo>> no_dt_adcs;
std::optional<std::vector<gpio_dt_spec>> no_dt_gpio;

// Only the channel list matters: the factory constructs devices, it does not initialize them.
AdcDTInfo FakeAdc(size_t channels) {
    AdcDTInfo info{};
    info.channel_configs.resize(channels);

    return info;
}

} // namespace

ZTEST_SUITE(value_randomizer_factories, NULL, NULL, NULL, NULL, NULL);

ZTEST(value_randomizer_factories, test_adc_without_hardware_gets_the_randomizer_instead_of_the_simulator) {
    AdcFactory factory([] -> std::optional<std::vector<AdcDTInfo>>& {
        return no_dt_adcs;
    });
    const auto manager = factory.Create();

    zassert_equal(manager->GetAdcCount(), 1);
    zassert_not_null(dynamic_cast<AdcRandomizer*>(manager->GetAdcs()[0].get()));
    zassert_is_null(dynamic_cast<AdcSimulator*>(manager->GetAdcs()[0].get()));
    zassert_equal(manager->GetChannelCount(), RANDOM_ADC_CHANNELS);
    zassert_true(manager->Initialize());
}

ZTEST(value_randomizer_factories, test_adc_without_a_provider_gets_the_randomizer) {
    AdcFactory factory(nullptr);
    const auto manager = factory.Create();

    zassert_equal(manager->GetAdcCount(), 1);
    zassert_not_null(dynamic_cast<AdcRandomizer*>(manager->GetAdcs()[0].get()));
}

ZTEST(value_randomizer_factories, test_adc_randomizer_channels_follow_the_hardware_channels) {
    std::optional<std::vector<AdcDTInfo>> hardware = std::vector<AdcDTInfo>{ FakeAdc(2), FakeAdc(3) };
    AdcFactory factory([&] -> std::optional<std::vector<AdcDTInfo>>& {
        return hardware;
    });
    const auto manager = factory.Create();

    zassert_equal(manager->GetAdcCount(), 3);
    zassert_not_null(dynamic_cast<Adc*>(manager->GetAdcs()[0].get()));
    zassert_not_null(dynamic_cast<Adc*>(manager->GetAdcs()[1].get()));
    zassert_not_null(dynamic_cast<AdcRandomizer*>(manager->GetAdcs()[2].get()));
    zassert_equal(manager->GetChannelCount(), 2 + 3 + RANDOM_ADC_CHANNELS);

    // Hardware keeps 0..4; global 5.. is the randomizer's local 0...
    zassert_equal(manager->Resolve(4).adc, manager->GetAdcs()[1].get());
    zassert_equal(manager->Resolve(4).local_channel, 2);
    zassert_equal(manager->Resolve(5).adc, manager->GetAdcs()[2].get());
    zassert_equal(manager->Resolve(5).local_channel, 0);
    zassert_equal(manager->Resolve(7).local_channel, 2);
}

ZTEST(value_randomizer_factories, test_adc_randomizer_voltages_use_the_default_pin_range) {
    AdcFactory factory(nullptr);
    const auto manager = factory.Create();
    manager->Initialize();

    const uint8_t channels[RANDOM_ADC_CHANNELS] = { 0, 1, 2 };
    AdcSample results[RANDOM_ADC_CHANNELS];

    for(int i = 0; i < 50; i++) {
        manager->GetAdcs()[0]->Sample({ .channels = channels, .results = results, .samples = 1 });

        for(const auto& result : results) {
            zassert_equal(result.error, AdcError::NONE);
            zassert_true(result.volts >= 0.0F && result.volts <= 3.3F);
        }

        k_sleep(K_MSEC(100));
    }
}

ZTEST(value_randomizer_factories, test_gpio_without_hardware_is_the_randomizer_alone) {
    GpioFactory factory([] -> std::optional<std::vector<gpio_dt_spec>>& {
        return no_dt_gpio;
    });
    const auto gpio = factory.Create();

    zassert_not_null(dynamic_cast<GpioRandomizer*>(gpio.get()));
    zassert_is_null(dynamic_cast<GpioSimulator*>(gpio.get()));
    zassert_equal(gpio->GetChannelCount(), RANDOM_GPIO_CHANNELS);
    zassert_equal(gpio->Initialize(), 0);
}

ZTEST(value_randomizer_factories, test_gpio_without_a_provider_is_the_randomizer_alone) {
    GpioFactory factory(nullptr);
    const auto gpio = factory.Create();

    zassert_not_null(dynamic_cast<GpioRandomizer*>(gpio.get()));
}

ZTEST(value_randomizer_factories, test_gpio_with_hardware_is_a_composite_that_keeps_the_hardware_numbers) {
    std::optional<std::vector<gpio_dt_spec>> hardware = std::vector<gpio_dt_spec>{ {}, {} };
    GpioFactory factory([&] -> std::optional<std::vector<gpio_dt_spec>>& {
        return hardware;
    });
    const auto gpio = factory.Create();

    zassert_not_null(dynamic_cast<CompositeGpio*>(gpio.get()));
    zassert_equal(gpio->GetChannelCount(), 2 + RANDOM_GPIO_CHANNELS);
}
