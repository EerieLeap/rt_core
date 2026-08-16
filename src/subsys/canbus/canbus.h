#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>

#include <subsys/threading/thread.h>

#include "canbus_type.h"
#include "can_frame.h"

namespace eerie_leap::subsys::canbus {

using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

using CanFrameHandler = std::function<void (const CanFrame&)>;

// The header is pulled in by domains that can be built without the subsystem.
#ifndef CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE
#define CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE 32
#endif

enum class CanbusState {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING
};

struct CanbusConfig {
    const device *canbus_dev;
    CanbusType type;
    uint32_t bitrate;
    uint32_t data_bitrate;
    bool is_extended_id;
    // Additional controller modes OR'ed into the base mode, e.g. CAN_MODE_LOOPBACK.
    can_mode_t extra_modes;

    CanbusConfig(
        const device *dev,
        CanbusType t,
        uint32_t br,
        uint32_t data_br = 0,
        bool ext_id = false,
        can_mode_t extra = 0)
        : canbus_dev(dev), type(t), bitrate(br), data_bitrate(data_br),
          is_extended_id(ext_id), extra_modes(extra) {}
};

class Canbus : public IThread {
public:
    using BitrateDetectedCallback = std::function<void (uint32_t bitrate)>;

    static constexpr int ERR_NOT_INITIALIZED = -1;
    static constexpr int ERR_FILTER_REJECTED = -2;
    static constexpr int ERR_TOO_MANY_HANDLERS = -3;
    static constexpr int ERR_INVALID_ARGUMENT = -4;

private:
    static constexpr int FRAME_MSGQ_SIZE = CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE;
    static constexpr int MSGQ_GET_TIMEOUT_MS = 10;
    static constexpr size_t MAX_HANDLERS_PER_FRAME_ID = 8;

    alignas(4) char frame_msgq_buffer_[FRAME_MSGQ_SIZE * sizeof(can_frame)];
    k_msgq frame_msgq_;

    // Guards config_, the filter/handler maps and bitrate_detected_fn_.
    // Never taken from the RX callback, which only feeds the message queue.
    mutable k_mutex lock_;

    CanbusConfig config_;
    std::unordered_map<uint32_t, int> can_filter_ids_; // <can_id, filter_id>
    std::unordered_map<uint32_t, can_filter> can_filters_; // <can_id, can_filter>
    std::unordered_map<uint32_t, std::unordered_map<int, CanFrameHandler>> handlers_; // <can_id, handlers>
    int next_handler_id_ = 1;

    atomic_t state_ = ATOMIC_INIT(static_cast<atomic_val_t>(CanbusState::STOPPED));
    atomic_t is_initialized_ = ATOMIC_INIT(0);
    atomic_t bitrate_detected_ = ATOMIC_INIT(0);
    atomic_t auto_detect_running_ = ATOMIC_INIT(0);
    atomic_t auto_detect_frame_count_ = ATOMIC_INIT(0);
    atomic_t rx_dropped_ = ATOMIC_INIT(0);
    BitrateDetectedCallback bitrate_detected_fn_;

    static constexpr k_timeout_t FRAME_SEND_TIMEOUT = K_MSEC(2);
    static constexpr uint32_t AUTO_DETECT_SAMPLE_MS = 500;
    static constexpr uint32_t MIN_FRAMES_FOR_DETECTION = 3;

    // NOTE: Thread is used as a Bottom Half for IRQ processing
    // and should have highest priority, or have MetaIRQ priority level,
    // for that set CONFIG_NUM_METAIRQ_PRIORITIES to be > 0
    static constexpr int thread_priority_ = K_HIGHEST_THREAD_PRIO;
    static constexpr bool thread_is_cooperative_ = true;
    static constexpr int thread_stack_size_ = 2048;
    std::unique_ptr<Thread> thread_;

    // Ordered by most common first
    static constexpr std::array<uint32_t, 9> classical_can_supported_bitrates_ = {
        500000, 1000000, 250000, 125000, 100000, 83333, 50000, 20000, 10000,
    };

    // Ordered by most common first
    static constexpr std::array<uint32_t, 13> canfd_supported_bitrates_ = {
        500000, 1000000, 250000, 125000, 100000, 83333, 50000, 20000, 10000,
        2000000, 4000000, 5000000, 8000000
    };

    void ThreadEntry() override;
    void BitrateAutodetectTask();
    void ProcessFramesTask();
    void DispatchFrame(const CanFrame& frame);

    void SetState(CanbusState state) { atomic_set(&state_, static_cast<atomic_val_t>(state)); }

    bool ApplyMode(can_mode_t extra_modes);
    bool StartActivityMonitoring();
    void StopActivityMonitoring();
    bool AutoDetectBitrate();
    bool TestBitrate(uint32_t bitrate);
    void RemoveAllFilters();

    static void SendFrameCallback(const device* dev, int error, void* user_data);
    bool SetTiming(uint32_t bitrate) const;
    bool SetDataTiming(uint32_t bitrate) const;
    bool RegisterFilter(uint32_t can_id);
    static void CanFrameReceivedCallback(const device *dev, can_frame *frame, void *user_data);
    static void AutoDetectFrameCallback(const device *dev, can_frame *frame, void *user_data);
    static void BusStateChangedCallback(const device *dev, can_state state, can_bus_err_cnt err_cnt, void *user_data);
    void PrintCanLimits() const;
    void PrintCanFdLimits() const;

public:
    explicit Canbus(const CanbusConfig& config);
    ~Canbus() override;

    Canbus(const Canbus&) = delete;
    Canbus& operator=(const Canbus&) = delete;
    Canbus(Canbus&&) = delete;
    Canbus& operator=(Canbus&&) = delete;

    bool Initialize();
    bool Configure(const CanbusConfig& config);
    bool Start();
    bool Stop();

    int RegisterFrameReceivedHandler(uint32_t can_id, CanFrameHandler handler);
    bool RemoveFrameReceivedHandler(uint32_t can_id, int handler_id);

    CanbusType GetType() const;
    CanbusState GetState() const { return static_cast<CanbusState>(atomic_get(&state_)); }
    CanbusConfig GetConfig() const;
    bool SendFrame(uint32_t frame_id, std::span<const uint8_t> frame_data) const;
    uint32_t GetDetectedBitrate() const;
    bool IsBitrateDetected() const { return atomic_get(&bitrate_detected_) != 0; }
    uint32_t GetRxDroppedCount() const { return static_cast<uint32_t>(atomic_get(&rx_dropped_)); }
    void RegisterBitrateDetectedCallback(const BitrateDetectedCallback& callback);

    static bool IsBitrateSupported(CanbusType type, uint32_t bitrate);
    static uint32_t GetMaxDataLength(CanbusType type);
};

}  // namespace eerie_leap::subsys::canbus
