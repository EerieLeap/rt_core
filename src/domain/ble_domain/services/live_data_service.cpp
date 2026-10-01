#include <algorithm>

#include <zephyr/sys/util.h>

#include "subsys/threading/scoped_mutex.h"

#include "live_data_service.h"

namespace eerie_leap::domain::ble_domain::services {

using eerie_leap::subsys::threading::ScopedMutex;

LiveDataService::LiveDataService(
    std::shared_ptr<IBleNotifier> notifier,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
        : notifier_(std::move(notifier)),
        sensor_readings_frame_(std::move(sensor_readings_frame)) {

    k_mutex_init(&lock_);
    sample_work_.service = this;
    k_work_init_delayable(&sample_work_.work, SampleWorkHandler);
}

LiveDataService::~LiveDataService() {
    Unsubscribe();

    k_work_sync sync;
    k_work_cancel_delayable_sync(&sample_work_.work, &sync);
}

size_t LiveDataService::GetCapacity() const {
    const size_t size = notifier_->GetMaxNotificationSize();
    if(size < LiveDataEncoder::HEADER_SIZE)
        return 0;

    return std::min(MAX_SENSORS, (size - LiveDataEncoder::HEADER_SIZE) / LiveDataEncoder::ENTRY_SIZE);
}

LiveDataService::SubscribeResult LiveDataService::Subscribe(
    std::span<const uint32_t> sensor_id_hashes, uint32_t period_ms) {

    const size_t capacity = GetCapacity();
    if(capacity == 0)
        return SubscribeResult::NOT_LISTENING;
    if(sensor_id_hashes.size() > capacity)
        return SubscribeResult::TOO_MANY;

    {
        ScopedMutex guard(lock_);
        std::ranges::copy(sensor_id_hashes, sensor_id_hashes_.begin());
        sensor_count_ = sensor_id_hashes.size();
        period_ms_ = ClampPeriod(period_ms);
    }

    k_work_reschedule(&sample_work_.work, K_NO_WAIT);

    return SubscribeResult::OK;
}

void LiveDataService::Unsubscribe() {
    {
        ScopedMutex guard(lock_);
        sensor_count_ = 0;
    }

    // A notification lost to a disconnect never reports back.
    is_in_flight_ = false;
    k_work_cancel_delayable(&sample_work_.work);
}

void LiveDataService::OnNotificationSent() {
    is_in_flight_ = false;
}

bool LiveDataService::IsSubscribed() const {
    ScopedMutex guard(lock_);

    return sensor_count_ > 0;
}

void LiveDataService::SampleWorkHandler(k_work* work) {
    auto* sample_work = CONTAINER_OF(k_work_delayable_from_work(work), SampleWork, work);
    sample_work->service->Sample();
}

void LiveDataService::Sample() {
    ScopedMutex guard(lock_);

    if(sensor_count_ == 0)
        return;

    k_work_schedule(&sample_work_.work, K_MSEC(period_ms_));
    const uint8_t sequence = sequence_++;

    if(is_in_flight_) {
        dropped_++;
        return;
    }

    const auto values = std::span(values_).first(sensor_count_);
    sensor_readings_frame_->GetReadingValues(std::span(sensor_id_hashes_).first(sensor_count_), values);

    const size_t size = LiveDataEncoder::Encode(sequence, k_uptime_get_32(), values, buffer_);
    if(size == 0 || size > notifier_->GetMaxNotificationSize()) {
        dropped_++;
        return;
    }

    is_in_flight_ = true;
    if(notifier_->Notify(std::span(buffer_).first(size)) != 0) {
        is_in_flight_ = false;
        dropped_++;
    }
}

} // namespace eerie_leap::domain::ble_domain::services
