#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <vector>
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

using namespace eerie_leap::subsys::threading;

using CanFrameHandler = std::function<void (const CanFrame&)>;

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

    CanbusConfig(
        const device *dev,
        CanbusType t,
        uint32_t br,
        uint32_t data_br = 0,
        bool ext_id = false)
        : canbus_dev(dev), type(t), bitrate(br), data_bitrate(data_br), is_extended_id(ext_id) {}
};

class Canbus : public IThread {
private:
    struct IsrCanFrameWrapper {
        Canbus* canbus;
        can_frame frame;
    };

    static constexpr int FRAME_MSGQ_SIZE = 4;
    static constexpr int MSGQ_GET_TIMEOUT_MS = 10;
    char frame_msgq_buffer_[FRAME_MSGQ_SIZE * sizeof(IsrCanFrameWrapper)];
    k_msgq frame_msgq_;

    CanbusConfig config_;
    std::unordered_map<uint32_t, int> can_filter_ids_; // <can_id, filter_id>
    std::unordered_map<uint32_t, can_filter> can_filters_; // <can_id, can_filter>
    std::unordered_map<uint32_t, std::unordered_map<int, CanFrameHandler>> handlers_; // <can_id, handlers>

    CanbusState state_ = CanbusState::STOPPED;
    bool is_initialized_ = false;
    bool bitrate_detected_ = false;
    atomic_t auto_detect_running_ = ATOMIC_INIT(0);
    std::function<void (uint32_t bitrate)> bitrate_detected_fn_;

    static constexpr k_timeout_t FRAME_SEND_TIMEOUT_MS = K_MSEC(2);
    static constexpr uint32_t AUTO_DETECT_TIMEOUT_MS = 500;
    static constexpr uint32_t MIN_FRAMES_FOR_DETECTION = 3;

    // NOTE: Thread is used as a Bottom Half for IRQ processing
    // and should have highest priority, or have MetaIRQ priority level,
    // for that set CONFIG_NUM_METAIRQ_PRIORITIES to be > 0
    static constexpr int thread_priority_ = K_HIGHEST_THREAD_PRIO;
    static constexpr bool thread_is_cooperative_ = true;
    static constexpr int thread_stack_size_ = 2048;
    std::unique_ptr<Thread> thread_;
    atomic_t is_thread_running_ = ATOMIC_INIT(0);

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

    bool StartActivityMonitoring();
    void StopActivityMonitoring();
    bool AutoDetectBitrate();
    bool TestBitrate(uint32_t bitrate, uint32_t &frame_count) const;

    static void SendFrameCallback(const device* dev, int error, void* user_data);
    bool SetTiming(uint32_t bitrate) const;
    bool SetDataTiming(uint32_t bitrate) const;
    bool RegisterFilter(uint32_t can_id);
    static void CanFrameReceivedCallback(const device *dev, can_frame *frame, void *user_data);
    void PrintCanLimits();
    void PrintCanFdLimits();

public:
    using BitrateDetectedCallback = std::function<void (uint32_t bitrate)>;

    explicit Canbus(const CanbusConfig& config);
    ~Canbus();

    bool Initialize();
    bool Configure(const CanbusConfig& config);
    bool Start();
    bool Stop();

    int RegisterFrameReceivedHandler(uint32_t can_id, CanFrameHandler handler);
    bool RemoveFrameReceivedHandler(uint32_t can_id, int handler_id);

    CanbusType GetType() const { return config_.type; }
    CanbusState GetState() const { return state_; }
    const CanbusConfig& GetConfig() const { return config_; }
    void SendFrame(uint32_t frame_id, std::span<const uint8_t> frame_data) const;
    uint32_t GetDetectedBitrate() const { return config_.bitrate; }
    bool IsBitrateDetected() const { return bitrate_detected_; }
    void RegisterBitrateDetectedCallback(const BitrateDetectedCallback& callback);

    static bool IsBitrateSupported(CanbusType type, uint32_t bitrate);
};

}  // namespace eerie_leap::subsys::canbus
