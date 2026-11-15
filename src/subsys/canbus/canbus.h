#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <subsys/threading/service_base.h>
#include <subsys/threading/thread.h>

#include "can_filter.h"
#include "can_frame.h"
#include "can_id.h"
#include "canbus_rx_entry.h"
#include "canbus_time.h"
#include "canbus_type.h"
#include "i_can_frame_tap.h"

namespace eerie_leap::subsys::canbus {

using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::Thread;

using CanFrameHandler = std::function<void(const CanFrame&)>;

// The header is pulled in by domains that can be built without the subsystem.
#ifndef CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE
    #define CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE 32
#endif

#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SLOTS
    #define CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SLOTS 16
#endif

/** @brief Controller, bus type and bitrates of one CAN channel. */
struct CanbusConfig {
    const device* canbus_dev;
    CanbusType type;
    uint32_t bitrate;      ///< Nominal bitrate; 0 detects it from bus activity.
    uint32_t data_bitrate; ///< CAN FD data phase bitrate; 0 when unused.
    /// Additional controller modes OR'ed into the base mode, e.g. CAN_MODE_LOOPBACK.
    can_mode_t extra_modes;

    CanbusConfig(const device* dev, CanbusType t, uint32_t br, uint32_t data_br = 0, can_mode_t extra = 0) :
        canbus_dev(dev),
        type(t),
        bitrate(br),
        data_bitrate(data_br),
        extra_modes(extra) {}
};

class Canbus : public IThread, public ServiceBase<> {
public:
    using BitrateDetectedCallback = std::function<void(uint32_t bitrate)>;

    /** @name Handler registration results */
    ///@{
    static constexpr int ERR_NOT_INITIALIZED = -1;
    static constexpr int ERR_FILTER_REJECTED = -2;   ///< The driver has no free hardware filter.
    static constexpr int ERR_TOO_MANY_HANDLERS = -3; ///< MAX_HANDLERS_PER_FILTER reached.
    static constexpr int ERR_INVALID_ARGUMENT = -4;
    static constexpr int ERR_FILTER_OVERLAP = -5;   ///< Another filter accepts some of the same identifiers.
    static constexpr int ERR_TOO_MANY_FILTERS = -6; ///< MAX_MASK_FILTERS reached.
    ///@}

    /// Handlers that may share one filter.
    static constexpr size_t MAX_HANDLERS_PER_FILTER = 8;
    /// Distinct mask filters; exact filters are limited only by the driver.
    static constexpr size_t MAX_MASK_FILTERS = 4;

    static constexpr k_timeout_t DEFAULT_SEND_TIMEOUT = K_MSEC(2);

    /// Frames in flight the tap's echo of the unit's own frames can follow.
    static constexpr size_t TX_SLOTS = CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SLOTS;

private:
    static constexpr int FRAME_MSGQ_SIZE = CONFIG_EERIE_LEAP_CANBUS_RX_QUEUE_SIZE;
    static constexpr int MSGQ_GET_TIMEOUT_MS = 10;

    // Registrations never overlap, so every driver delivers a frame through at most one filter.
    struct FilterEntry {
        CanFilter filter;
        int filter_id = -1; // -1 while no hardware filter is installed
        std::unordered_map<int, CanFrameHandler> handlers;
    };

    // Entries come from the driver's context: received frames, the completions of the unit's own frames
    // (SendFrame() is const, so the queue is mutable) and state changes.
    alignas(8) char frame_msgq_buffer_[FRAME_MSGQ_SIZE * sizeof(CanbusRxEntry)] = {};
    mutable k_msgq frame_msgq_{};

    // Guards config_, the filter entries, bitrate_detected_fn_, the tap and its filters.
    // Never taken from the RX callback, which only feeds the message queue.
    mutable k_mutex lock_{};

    CanbusConfig config_;
    std::unordered_map<uint32_t, FilterEntry> exact_entries_; // <CanId::Key(), entry>
    std::array<std::optional<FilterEntry>, MAX_MASK_FILTERS> mask_entries_;
    int next_handler_id_ = 1;

    atomic_t is_initialized_ = ATOMIC_INIT(0);
    atomic_t bitrate_detected_ = ATOMIC_INIT(0);
    atomic_t auto_detect_running_ = ATOMIC_INIT(0);
    atomic_t auto_detect_frame_count_ = ATOMIC_INIT(0);
    atomic_t rx_dropped_ = ATOMIC_INIT(0);
    BitrateDetectedCallback bitrate_detected_fn_;

    // The state change callback has no user data, so the driver's node is the first member of a
    // standard-layout struct that adds the bus, and the callback casts the node back.
    struct StateChangeNode {
        can_state_change_callback callback{};
        Canbus* canbus = nullptr;
    };
    static_assert(std::is_standard_layout_v<StateChangeNode>);

    // Registered on the configured device in Configure() and removed in the destructor; the
    // driver keeps a pointer to it, so it lives in the object, never on a stack.
    StateChangeNode state_change_node_{};
    const device* state_change_callback_dev_ = nullptr;

    /// A frame the unit sent, kept until the controller completes it: the completion callback gets no frame.
    struct TxSlot {
        const Canbus* owner = nullptr;
        can_frame frame{};
    };

    static constexpr atomic_val_t FILTER_SWAP_NONE = 0;
    static constexpr atomic_val_t FILTER_SWAP_REPEATED = 1;
    static constexpr atomic_val_t FILTER_SWAP_MAY_HAVE_MISSED = 2;

    // The tap and its two accept-all hardware filters (standard, extended); guarded by lock_.
    ICanFrameTap* tap_ = nullptr;
    std::array<int, 2> tap_filter_ids_ = { -1, -1 };
    uint64_t filter_swap_time_us_ = 0;
    // tap_ != nullptr, readable without lock_ so the receive thread takes the lock only while a tap is set.
    atomic_t tap_active_ = ATOMIC_INIT(0);
    // The outcome of the swap SetTap() made, which the receive thread announces to the tap and clears.
    atomic_t filter_swap_ = ATOMIC_INIT(FILTER_SWAP_NONE);

    // Allocated by the first SetTap() under lock_, then fixed until the destructor frees it.
    std::unique_ptr<TxSlot[]> tx_slots_;
    mutable atomic_t tx_slots_used_[ATOMIC_BITMAP_SIZE(TX_SLOTS)] = {};
    mutable atomic_t tx_echo_lost_ = ATOMIC_INIT(0);

    void RegisterStateChangeCallback();
    void UnregisterStateChangeCallback();

    static constexpr uint32_t AUTO_DETECT_SAMPLE_MS = 500;
    static constexpr uint32_t MIN_FRAMES_FOR_DETECTION = 3;

    // NOTE: Thread is used as a Bottom Half for IRQ processing
    // and should have highest priority, or have MetaIRQ priority level,
    // for that set CONFIG_NUM_METAIRQ_PRIORITIES to be > 0
    static constexpr int THREAD_PRIORITY = K_HIGHEST_THREAD_PRIO;
    static constexpr bool THREAD_IS_COOPERATIVE = true;
    static constexpr int THREAD_STACK_SIZE = 2048;
    std::unique_ptr<Thread> thread_;

    // Ordered by most common first
    static constexpr std::array<uint32_t, 9> CLASSICAL_CAN_SUPPORTED_BITRATES = {
        500000,
        1000000,
        250000,
        125000,
        100000,
        83333,
        50000,
        20000,
        10000,
    };

    // Ordered by most common first
    static constexpr std::array<uint32_t, 13> CANFD_SUPPORTED_BITRATES = { 500000, 1000000, 250000, 125000, 100000,
        83333, 50000, 20000, 10000, 2000000, 4000000, 5000000, 8000000 };

    void ThreadEntry() override;
    void BitrateAutodetectTask();
    void ProcessFramesTask();
    void DispatchFrame(const CanFrame& frame);
    void ProcessReceivedEntry(const CanbusRxEntry& entry);
    void ProcessTxEchoEntry(const CanbusRxEntry& entry);
    template<typename Call>
    void WithTap(Call&& call);

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

    bool ApplyMode(can_mode_t extra_modes);
    bool StartActivityMonitoring();
    void StopActivityMonitoring();
    bool AutoDetectBitrate();
    bool TestBitrate(uint32_t bitrate);

    // The entry helpers below require lock_.
    FilterEntry* FindEntry(const CanFilter& filter);
    FilterEntry* FindEntryByHandler(int handler_id);
    FilterEntry* FindMatchingEntry(const CanId& can_id);
    FilterEntry* CreateEntry(const CanFilter& filter);
    void EraseEntry(const CanFilter& filter);
    bool HasOverlap(const CanFilter& filter) const;
    bool InstallFilter(FilterEntry& entry);
    void UninstallFilter(FilterEntry& entry) const;
    bool InstallDeferredFilters();
    void RemoveAllFilters();
    void ClearEntries();
    bool IsDetectingBitrate() const;
    bool AddTapFilters();
    void RemoveTapFilters();
    bool HasTapFilters() const;
    void ClearTap();
    TxSlot* TakeTxSlot() const;
    void ReleaseTxSlot(TxSlot* slot) const;
    void ResetTxSlots() const;

    static void SendFrameCallback(const device* dev, int error, void* user_data);
    bool SetTiming(uint32_t bitrate) const;
    bool SetDataTiming(uint32_t bitrate) const;
    static void CanFrameReceivedCallback(const device* dev, can_frame* frame, void* user_data);
    static void AutoDetectFrameCallback(const device* dev, can_frame* frame, void* user_data);
    static void BusStateChangedCallback(
        const device* dev,
        can_state_change_callback* callback,
        can_state state,
        can_bus_err_cnt err_cnt
    );
    void PrintCanLimits() const;
    void PrintCanFdLimits() const;

public:
    explicit Canbus(const CanbusConfig& config);
    ~Canbus() override;

    Canbus(const Canbus&) = delete;
    Canbus& operator=(const Canbus&) = delete;
    Canbus(Canbus&&) = delete;
    Canbus& operator=(Canbus&&) = delete;

    bool Configure(const CanbusConfig& config);

    /**
     * @brief Calls @p handler from the RX thread for every frame that passes @p filter.
     *
     * A CanId registers an exact-match filter. Identical filters share one hardware filter.
     *
     * @return A positive handler ID, or one of the ERR_* codes. ERR_FILTER_OVERLAP means another
     *         registered filter accepts some of the same identifiers.
     */
    int RegisterFrameReceivedHandler(const CanFilter& filter, CanFrameHandler handler);

    /**
     * @brief Removes a handler; the hardware filter goes with its last handler.
     *
     * Waits for a dispatch in progress, so the handler is not running once this returns.
     */
    bool RemoveFrameReceivedHandler(int handler_id);

    /**
     * @brief Sets the tap that sees every frame the bus receives and every frame the unit sends. One a bus.
     *
     * The receive thread calls the tap under the bus's lock, before it dispatches a received frame, so
     * clearing it (nullptr) waits for a call in progress and no call follows once this returns.
     *
     * Setting a tap swaps the hardware filters: two accept-all filters (standard and extended) go in, then
     * the registered handlers' filters go out, and the receive thread matches frames to handlers in
     * software, so handlers get the same frames. A driver that calls every matching filter (Zephyr's
     * SJA1000 core, the loopback driver) delivers a frame twice while both sets are installed, so frames
     * around the swap may repeat. Where the driver has no room for the new filters, the old ones go first
     * and frames may go missing; the tap hears which in OnFilterSwap(). Clearing the tap reverses the swap.
     * Handlers registered meanwhile get no hardware filter until the tap goes.
     *
     * While the bitrate is being detected, the tap is only recorded: the accept-all filters go in when
     * detection succeeds, followed by an OnState() call with the controller's state.
     *
     * Remote frames reach the tap and never the handlers. Stopping or reconfiguring the bus clears the tap.
     *
     * @return false when the bus is not initialized, another tap is set, or the driver rejected the
     *         filters (everything is then as before). Clearing always succeeds.
     */
    bool SetTap(ICanFrameTap* tap);

    /// The time source of every time the bus stamps; see canbus_time.h.
    static uint64_t NowUs() {
        return eerie_leap::subsys::canbus::NowUs();
    }

    /**
     * @brief Queues a change of the controller's state for the tap and logs it.
     *
     * What the driver's state change callback calls, in the driver's context; public because the loopback
     * driver never changes state, so tests call it. A full queue drops the entry.
     */
    void OnBusStateChanged(can_state state, can_bus_err_cnt err_cnt);

    CanbusType GetType() const;
    CanbusConfig GetConfig() const;

    /**
     * @brief Sends one frame without holding the handler lock.
     *
     * Every frame on a CAN FD channel goes out as an FD frame with bit rate switch, its length rounded
     * up to the next DLC (the padding is zero); a classical channel sends a classical frame. IDE comes
     * from @p frame_id. To send a frame as it is, use the overload that takes a CanFrame.
     *
     * While a tap is set the tap sees the frame again as a transmitted one when the controller completes
     * it; with all TX_SLOTS in flight the frame goes out without that echo (GetTxEchoLostCount()).
     *
     * @param timeout How long to wait for space in the TX queue; K_NO_WAIT never blocks.
     * @return 0, -ENETDOWN when the bus is not running, -EINVAL for a malformed frame,
     *         -EAGAIN when the TX queue stayed full for the timeout, or another driver error.
     */
    int SendFrame(
        const CanId& frame_id,
        std::span<const uint8_t> frame_data,
        k_timeout_t timeout = DEFAULT_SEND_TIMEOUT
    ) const;

    /**
     * @brief Sends @p frame with its own IDE, FDF, BRS and RTR; the ESI is the controller's to set.
     *
     * The DLC is @c frame.dlc (at most 8) for a remote frame and the payload's for any other. The tap's
     * echo works as for the other overload. Nothing is logged above debug level for a refusal, because
     * the CAN monitor may send many frames.
     *
     * @return 0, -ENETDOWN when the bus is not running, or -EAGAIN or another driver error as above.
     *         -EINVAL for:
     *         - an FD frame on a classical channel;
     *         - BRS without FDF;
     *         - a remote frame with FDF (CAN FD has none), with a payload or with a DLC above 8;
     *         - more than 8 bytes without FDF;
     *         - a length no DLC gives (FD: 0 to 8, 12, 16, 20, 24, 32, 48 or 64);
     *         - an ID that does not fit its format.
     */
    int SendFrame(const CanFrame& frame, k_timeout_t timeout = DEFAULT_SEND_TIMEOUT) const;
    uint32_t GetDetectedBitrate() const;
    bool IsBitrateDetected() const {
        return atomic_get(&bitrate_detected_) != 0;
    }
    uint32_t GetRxDroppedCount() const {
        return static_cast<uint32_t>(atomic_get(&rx_dropped_));
    }
    /// Frames sent while a tap was set whose echo never reached the tap: no free slot, or a full queue.
    uint32_t GetTxEchoLostCount() const {
        return static_cast<uint32_t>(atomic_get(&tx_echo_lost_));
    }
    void RegisterBitrateDetectedCallback(const BitrateDetectedCallback& callback);

    static bool IsBitrateSupported(CanbusType type, uint32_t bitrate);
    static uint32_t GetMaxDataLength(CanbusType type);
};

} // namespace eerie_leap::subsys::canbus
