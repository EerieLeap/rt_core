#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/canbus/can_frame.h"
#include "subsys/time/i_time_service.h"
#include "domain/canbus_domain/models/can_message_configuration.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/isr_sensor_readers/can_route_table.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::isr_sensor_readers;

ZTEST_SUITE(can_route_table, NULL, NULL, NULL, NULL, NULL);

namespace {

// The unit suite does not build the time subsystem; readings only need a timestamp to be present.
class FakeTimeService : public ITimeService {
public:
    std::chrono::system_clock::time_point GetCurrentTime() override {
        return std::chrono::system_clock::time_point(std::chrono::seconds(42));
    }

    std::chrono::system_clock::time_point GetTimeSinceBoot() override {
        return std::chrono::system_clock::time_point(std::chrono::seconds(1));
    }
};

constexpr uint8_t kBus = 0;
constexpr uint32_t kSignalsFrameId = 0x100;
constexpr uint32_t kRawFrameId = 0x200;
constexpr size_t kSignalCount = 16;

struct Fixture {
    std::shared_ptr<CanMessageConfiguration> message;
    std::vector<std::shared_ptr<Sensor>> sensors;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<FakeTimeService> time_service;

    CanRouteTable::SignalResolver Resolver() const {
        return [this](uint8_t bus, uint32_t frame_id, uint32_t signal_name_hash) -> std::shared_ptr<const CanSignalConfiguration> {
            if(bus != kBus || frame_id != message->frame_id)
                return nullptr;

            const auto* signal = message->TryGetSignal(signal_name_hash);
            if(signal == nullptr)
                return nullptr;

            return std::shared_ptr<const CanSignalConfiguration>(message, signal);
        };
    }
};

// 16 unsigned nibbles across one 8-byte frame, plus one raw frame sensor on another ID.
Fixture MakeFixture() {
    Fixture fixture;
    auto* mr = Mrm::GetDefaultPmr();

    fixture.message = make_shared_pmr<CanMessageConfiguration>(mr);
    fixture.message->frame_id = kSignalsFrameId;
    fixture.message->message_size = 8;

    for(size_t i = 0; i < kSignalCount; i++) {
        CanSignalConfiguration signal(std::allocator_arg, mr);
        signal.start_bit = i * 4;
        signal.size_bits = 4;
        signal.is_signed = false;
        signal.SetName("sig_" + std::to_string(i));
        fixture.message->signal_configurations.push_back(std::move(signal));
    }

    for(size_t i = 0; i < kSignalCount; i++) {
        auto sensor = std::make_shared<Sensor>(std::allocator_arg, mr, "can_" + std::to_string(i));
        sensor->configuration.type = SensorType::CANBUS_ANALOG;
        sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(mr, kBus, kSignalsFrameId, "sig_" + std::to_string(i));
        sensor->configuration.UpdateConnectionString();
        fixture.sensors.push_back(sensor);
    }

    auto raw = std::make_shared<Sensor>(std::allocator_arg, mr, "raw");
    raw->configuration.type = SensorType::CANBUS_RAW;
    raw->configuration.canbus_source = make_unique_pmr<CanbusSource>(mr, kBus, kRawFrameId);
    raw->configuration.UpdateConnectionString();
    fixture.sensors.push_back(raw);

    fixture.frame = std::make_shared<SensorReadingsFrame>();
    fixture.generation = SensorPipelineBuilder(nullptr, fixture.frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(fixture.sensors));

    fixture.time_service = std::make_shared<FakeTimeService>();

    return fixture;
}

} // namespace

ZTEST(can_route_table, test_Build_groups_the_sensors_of_a_frame_into_one_route) {
    auto fixture = MakeFixture();

    CanRouteTable table;
    table.Build(*fixture.generation, fixture.Resolver());

    const auto routes = table.GetRoutes();
    zassert_equal(routes.size(), 2, "One route per (bus, frame ID), however many sensors read the frame");

    const CanRoute* signals = nullptr;
    const CanRoute* raw = nullptr;
    for(const auto& route : routes) {
        if(route.frame_id == kSignalsFrameId) signals = &route;
        if(route.frame_id == kRawFrameId) raw = &route;
    }

    zassert_not_null(signals);
    zassert_not_null(raw);
    zassert_equal(signals->bus_channel, kBus);
    zassert_equal(signals->bindings.size(), kSignalCount, "No limit on signals per frame");
    zassert_equal(raw->bindings.size(), 1);
    zassert_is_null(raw->bindings[0].signal, "A raw sensor has no signal to decode");

    for(const auto& binding : signals->bindings)
        zassert_not_null(binding.signal);
}

ZTEST(can_route_table, test_Build_leaves_out_a_sensor_whose_signal_is_not_configured) {
    auto fixture = MakeFixture();

    CanRouteTable table;
    table.Build(*fixture.generation, [](uint8_t, uint32_t, uint32_t) { return nullptr; });

    zassert_equal(table.GetRoutes().size(), 1, "Only the raw sensor needs no signal");
    zassert_equal(table.GetRoutes()[0].frame_id, kRawFrameId);
}

ZTEST(can_route_table, test_Dispatch_decodes_every_signal_of_the_frame_once) {
    auto fixture = MakeFixture();

    CanRouteTable table;
    table.Build(*fixture.generation, fixture.Resolver());

    const CanRoute* signals = nullptr;
    for(const auto& route : table.GetRoutes())
        if(route.frame_id == kSignalsFrameId) signals = &route;

    // Nibble i holds the value i, little-endian.
    CanFrame frame;
    frame.id = kSignalsFrameId;
    frame.data = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };

    std::array<std::optional<float>, kSignalCount> values;
    size_t readings = 0;

    CanRouteTable::Dispatch(*signals, frame, *fixture.time_service,
        [&](const SensorRuntime& runtime, SensorReading& reading) {
            readings++;

            zassert_equal(reading.source, ReadingSource::ISR);
            zassert_equal(reading.status, ReadingStatus::RAW);
            zassert_true(reading.timestamp.has_value());
            zassert_true(reading.raw_value.has_value());

            // The sensor's position in the generation is also its signal index.
            values[runtime.slot.index] = reading.value;
        });

    zassert_equal(readings, kSignalCount, "One reading per sensor on the frame");
    for(size_t i = 0; i < kSignalCount; i++) {
        zassert_true(values[i].has_value(), "Signal %zu was not decoded", i);
        zassert_equal(values[i].value(), static_cast<float>(i), "Signal %zu decoded to %f", i, static_cast<double>(values[i].value()));
    }
}

ZTEST(can_route_table, test_Dispatch_hands_a_raw_sensor_the_frame_itself) {
    auto fixture = MakeFixture();

    CanRouteTable table;
    table.Build(*fixture.generation, fixture.Resolver());

    const CanRoute* raw = nullptr;
    for(const auto& route : table.GetRoutes())
        if(route.frame_id == kRawFrameId) raw = &route;

    CanFrame frame;
    frame.id = kRawFrameId;
    frame.data = { 1, 2, 3 };

    size_t readings = 0;
    CanRouteTable::Dispatch(*raw, frame, *fixture.time_service,
        [&](const SensorRuntime& runtime, SensorReading& reading) {
            readings++;

            zassert_equal(runtime.GetSensor().configuration.type, SensorType::CANBUS_RAW);
            zassert_equal(reading.can_frame, &frame);
            zassert_false(reading.value.has_value());

            // Committing stores the frame for the loggers.
            fixture.frame->AddOrUpdateReading(reading);
        });

    zassert_equal(readings, 1);

    CanFrame stored;
    zassert_true(fixture.frame->TryGetCanFrame(fixture.sensors.back()->id_hash, stored));
    zassert_equal(stored.data.size(), 3);
}

ZTEST(can_route_table, test_Dispatch_ignores_an_empty_frame) {
    auto fixture = MakeFixture();

    CanRouteTable table;
    table.Build(*fixture.generation, fixture.Resolver());

    CanFrame frame;
    frame.id = kSignalsFrameId;

    size_t readings = 0;
    for(const auto& route : table.GetRoutes())
        CanRouteTable::Dispatch(route, frame, *fixture.time_service, [&](const SensorRuntime&, SensorReading&) { readings++; });

    zassert_equal(readings, 0);
}
