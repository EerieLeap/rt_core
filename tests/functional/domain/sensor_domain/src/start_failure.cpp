// A source that throws while the processing service starts leaves nothing behind: the sources
// attached before it are detached and the generation is dropped.
#include <memory>
#include <stdexcept>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/gpio/i_gpio.h"

#include "processing_test_support.h"

using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::domain::sensor_domain::models;

using namespace processing_test_support;

ZTEST_SUITE(start_failure, NULL, NULL, NULL, NULL, NULL);

namespace {

// Counts the handlers it holds; the registration numbered refused_registration (from 0) throws
// something the ISR reader factory does not catch. Readers are created in processing order, not
// configuration order, so the failure is picked by count rather than by channel.
class FakeGpio : public IGpio {
public:
    int refused_registration = -1;
    int registrations = 0;
    int handlers = 0;
    int next_handler_id = 1;

    int Initialize() override {
        return 0;
    }
    bool ReadChannel(int channel) override {
        ARG_UNUSED(channel);
        return false;
    }
    int GetChannelCount() override {
        return 4;
    }

    int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) override {
        ARG_UNUSED(channel);
        ARG_UNUSED(edge);
        ARG_UNUSED(handler);

        if(registrations++ == refused_registration)
            throw std::logic_error("GPIO channel refused");

        handlers++;
        return next_handler_id++;
    }

    bool RemoveChannelChangedHandler(int channel, int handler_id) override {
        ARG_UNUSED(channel);
        ARG_UNUSED(handler_id);

        handlers--;
        return true;
    }
};

std::shared_ptr<Sensor> MakeIndicatorSensor(std::string_view id, int channel) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::PHYSICAL_INDICATOR;
    sensor->configuration.channel = channel;

    return sensor;
}

} // namespace

ZTEST(start_failure, test_a_source_that_throws_leaves_nothing_running) {
    const auto fs_service = MakeFormattedFsService();

    const auto gpio = std::make_shared<FakeGpio>();
    // The first reader attaches, the second throws.
    gpio->refused_registration = 1;

    const auto rig = MakeProcessingRig(
        fs_service, nullptr, nullptr,
        {
            MakeIndicatorSensor("first", 0),
            MakeIndicatorSensor("second", 1),
            MakeVirtualSensor("polled", "40 + 2", 20),
        },
        0, gpio
    );

    bool is_thrown = false;
    try {
        rig.processing_service->Start();
    } catch(const std::logic_error&) {
        is_thrown = true;
    }

    zassert_true(is_thrown, "The source's exception reaches the caller");
    zassert_equal(gpio->registrations, 2, "One reader attached before the failure");
    zassert_true(rig.processing_service->IsStopped());
    zassert_is_null(rig.processing_service->GetGeneration().get());
    zassert_equal(gpio->handlers, 0, "The reader attached before the failure was detached");

    k_msleep(100);

    zassert_false(rig.frame->TryGetReading("first").has_value(), "Nothing delivers into the dropped generation");
    zassert_false(rig.frame->TryGetReading("second").has_value());
    zassert_false(rig.frame->TryGetReading("polled").has_value());

    // The failure left nothing that stops a later start.
    gpio->refused_registration = -1;
    zassert_true(rig.processing_service->Start());
    zassert_equal(gpio->handlers, 2);

    k_msleep(100);
    zassert_true(rig.frame->TryGetReading("polled").has_value());

    zassert_true(rig.processing_service->Stop());
    zassert_equal(gpio->handlers, 0);
}
