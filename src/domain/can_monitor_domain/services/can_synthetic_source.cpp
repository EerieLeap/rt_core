#include "can_synthetic_source.h"

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC

    #include <algorithm>
    #include <array>
    #include <span>

    #include <zephyr/drivers/can.h>
    #include <zephyr/kernel.h>

    #include "subsys/canbus/can_frame.h"
    #include "subsys/canbus/canbus_time.h"
    #include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::subsys::canbus::CAN_FRAME_MAX_DATA_LENGTH;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::NowUs;
using eerie_leap::utilities::memory::Mrm;

namespace {

constexpr uint32_t EXTENDED_ID_BASE = 0x18DA0000;
constexpr uint64_t SECOND_US = 1000000;
constexpr uint8_t PATTERN = 0xA5;

} // namespace

CanSyntheticSource::CanSyntheticSource(CanMonitorSession& session, const CanSyntheticParameters& parameters) :
    session_(session),
    parameters_(parameters) {}

CanSyntheticSource::~CanSyntheticSource() {
    Stop();
}

uint32_t CanSyntheticSource::IdOf(const CanSyntheticParameters& parameters, uint64_t number) {
    const auto index = static_cast<uint32_t>(number % parameters.ids);

    return parameters.ext ? EXTENDED_ID_BASE + index : index;
}

uint8_t CanSyntheticSource::PayloadByte(uint64_t number, size_t index) {
    if(index < sizeof(uint64_t))
        return static_cast<uint8_t>(number >> (8 * index));

    return static_cast<uint8_t>(PATTERN ^ index);
}

bool CanSyntheticSource::Start() {
    if(thread_ && thread_->IsRunning())
        return true;

    is_stopping_ = false;

    if(!thread_) {
        // External RAM: the kernel heap of a display unit has no 8 KB to spare for a bench thread.
        thread_ = std::make_unique<Thread>(
            "can_synthetic", this, THREAD_STACK_SIZE, THREAD_PRIORITY, false, Mrm::GetExtPmr()
        );
        if(!thread_->Initialize()) {
            thread_.reset();
            return false;
        }
    }

    return thread_->Start();
}

void CanSyntheticSource::Stop() {
    if(!thread_)
        return;

    is_stopping_ = true;
    thread_->Join();
}

void CanSyntheticSource::WriteFrame(uint64_t number) const {
    CanFrame frame;
    frame.id = IdOf(parameters_, number);
    frame.is_extended = parameters_.ext;
    frame.is_can_fd = parameters_.fd;
    frame.is_bitrate_switch = parameters_.fd;
    frame.dlc = parameters_.dlc;

    std::array<uint8_t, CAN_FRAME_MAX_DATA_LENGTH> payload{};
    const size_t length = std::min<size_t>(can_dlc_to_bytes(parameters_.dlc), payload.size());
    for(size_t i = 0; i < length; ++i)
        payload[i] = PayloadByte(number, i);
    frame.data.Assign(std::span<const uint8_t>(payload).first(length));

    frame.time_us = NowUs();
    session_.OnFrame(frame, frame.time_us, false, 0);
}

void CanSyntheticSource::ThreadEntry() {
    const uint64_t start_us = NowUs();
    uint64_t written = 0;

    while(!is_stopping_.load(std::memory_order_relaxed)) {
        // Frame k is due k/fps after the start: the count due is 1 + elapsed * fps, split so that it
        // can't overflow.
        const uint64_t elapsed_us = NowUs() - start_us;
        const uint64_t due = 1 + (elapsed_us / SECOND_US) * parameters_.fps
            + ((elapsed_us % SECOND_US) * parameters_.fps) / SECOND_US;

        while(written < due && !is_stopping_.load(std::memory_order_relaxed)) {
            WriteFrame(written++);
            generated_.store(static_cast<uint32_t>(written), std::memory_order_relaxed);
        }

        const uint64_t next_us = start_us + (written * SECOND_US) / parameters_.fps;
        const uint64_t now_us = NowUs();
        const uint64_t wait_us = next_us > now_us ? next_us - now_us : 0;

        k_usleep(
            std::clamp<int32_t>(
                static_cast<int32_t>(std::min<uint64_t>(wait_us, MAX_SLEEP_US)), MIN_SLEEP_US, MAX_SLEEP_US
            )
        );
    }
}

} // namespace eerie_leap::domain::can_monitor_domain::services

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
