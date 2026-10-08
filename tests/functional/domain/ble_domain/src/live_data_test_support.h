#pragma once

#include <bit>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/bluetooth/i_ble_notifier.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/ble_domain/utilities/live_data_encoder.h"
#include "domain/ble_domain/services/live_data_service.h"

namespace live_data_test {

using namespace eerie_leap::domain::sensor_domain::models;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::bluetooth::IBleNotifier;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::ble_domain::utilities::LiveDataEncoder;
using eerie_leap::domain::ble_domain::services::LiveDataService;

using Bytes = std::vector<uint8_t>;

// The `live` characteristic of a central: records notifications, or fails them on demand.
class FakeNotifier : public IBleNotifier {
private:
    mutable k_mutex lock_{};
    size_t max_size_ = 244;
    int result_ = 0;
    std::vector<Bytes> notifications_;

public:
    // Called after each accepted notification, e.g. to report it sent.
    std::function<void()> on_notified;

    FakeNotifier() { k_mutex_init(&lock_); }

    void SetMaxSize(size_t size) {
        k_mutex_lock(&lock_, K_FOREVER);
        max_size_ = size;
        k_mutex_unlock(&lock_);
    }

    void SetResult(int result) {
        k_mutex_lock(&lock_, K_FOREVER);
        result_ = result;
        k_mutex_unlock(&lock_);
    }

    [[nodiscard]] size_t GetMaxNotificationSize() const override {
        k_mutex_lock(&lock_, K_FOREVER);
        const size_t size = max_size_;
        k_mutex_unlock(&lock_);

        return size;
    }

    int Notify(std::span<const uint8_t> data) override {
        k_mutex_lock(&lock_, K_FOREVER);
        const int result = result_;
        if(result == 0)
            notifications_.emplace_back(data.begin(), data.end());
        k_mutex_unlock(&lock_);

        if(result == 0 && on_notified)
            on_notified();

        return result;
    }

    [[nodiscard]] std::vector<Bytes> Notifications() const {
        k_mutex_lock(&lock_, K_FOREVER);
        auto notifications = notifications_;
        k_mutex_unlock(&lock_);

        return notifications;
    }

    void Clear() {
        k_mutex_lock(&lock_, K_FOREVER);
        notifications_.clear();
        k_mutex_unlock(&lock_);
    }

    std::vector<Bytes> WaitForNotifications(size_t count, int timeout_ms = 1000) const {
        for(int waited_ms = 0; waited_ms < timeout_ms; waited_ms += 10) {
            auto notifications = Notifications();
            if(notifications.size() >= count)
                return notifications;

            k_msleep(10);
        }

        return Notifications();
    }
};

struct DecodedSample {
    uint8_t version;
    uint8_t sequence;
    uint32_t time_ms;
    std::vector<std::pair<uint8_t, float>> values;
};

inline DecodedSample Decode(std::span<const uint8_t> data) {
    zassert_true(data.size() >= LiveDataEncoder::HEADER_SIZE);

    DecodedSample sample{
        .version = data[0],
        .sequence = data[1],
        .time_ms = sys_get_le32(&data[2]),
        .values = {},
    };

    const uint8_t count = data[6];
    zassert_equal(data.size(), LiveDataEncoder::GetMaxSize(count));

    for(size_t i = 0; i < count; i++) {
        const uint8_t* entry = &data[LiveDataEncoder::HEADER_SIZE + i * LiveDataEncoder::ENTRY_SIZE];
        sample.values.emplace_back(entry[0], std::bit_cast<float>(sys_get_le32(entry + 1)));
    }

    return sample;
}

// A frame with processed values for some sensors.
struct Readings {
    std::shared_ptr<SensorReadingsFrame> frame = std::make_shared<SensorReadingsFrame>();
    std::vector<std::shared_ptr<Sensor>> sensors;
    std::vector<std::optional<float>> values;

    uint32_t Add(std::string_view id, std::optional<float> value) {
        auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
        sensors.push_back(sensor);
        values.push_back(value);

        // Slots exist per configuration, so every added sensor reconfigures the frame and replays the readings.
        frame->Configure(sensors);
        for(size_t i = 0; i < sensors.size(); i++) {
            SensorReading reading(sensors[i].get());
            reading.source = ReadingSource::PROCESSING;
            if(values[i].has_value()) {
                reading.status = ReadingStatus::PROCESSED;
                reading.value = *values[i];
            }
            frame->AddOrUpdateReading(reading);
        }

        return sensor->id_hash;
    }
};

} // namespace live_data_test
