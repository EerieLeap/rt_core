#pragma once

#include <cstdint>

namespace eerie_leap::subsys::bluetooth::access_point {

/**
 * @brief When the access point asks for the fast and the idle connection parameters.
 *
 * Events go in and decisions come out; nothing here touches Bluetooth or the kernel, so a unit
 * test can drive it. The caller serializes the events and carries the decisions out in the order
 * it got them.
 *
 * The link is fast while SMP writes arrive and while any hold is held (the CAN monitor's streams
 * make no SMP writes), and idle again some time after the last of them. A fast request that leaves
 * the interval above 15 ms is followed once by a request for 15 to 30 ms: Apple's accessory
 * guidelines ask for 15 ms or more, and some centrals refuse the 7.5 ms minimum.
 */
class FastLinkPolicy {
public:
    /// The longest interval that needs no fallback, 15 ms in units of 1.25 ms.
    static constexpr uint16_t FALLBACK_THRESHOLD_UNITS = 12;

    enum class LinkRequest : uint8_t {
        /// Nothing to request.
        NONE,
        /// Request the fast parameters and the 2M PHY, then report OnFastRequested().
        FAST,
        /// Request the idle parameters.
        IDLE,
    };

    enum class IdleWork : uint8_t {
        /// Leave the idle work as it is.
        KEEP,
        /// (Re)start the idle work's delay.
        SCHEDULE,
        /// Cancel the idle work.
        CANCEL,
    };

    struct Decision {
        LinkRequest request = LinkRequest::NONE;
        IdleWork idle_work = IdleWork::KEEP;
        /// Request 15 to 30 ms.
        bool request_fallback = false;

        bool operator==(const Decision&) const = default;
    };

private:
    uint32_t holds_ = 0;
    // The fast parameters are requested and the idle ones have not been since.
    bool is_fast_ = false;
    // A fast request was made and no update has answered it yet.
    bool is_fallback_armed_ = false;

public:
    [[nodiscard]] uint32_t GetHoldCount() const {
        return holds_;
    }
    [[nodiscard]] bool IsFast() const {
        return is_fast_;
    }
    [[nodiscard]] bool IsFallbackArmed() const {
        return is_fallback_armed_;
    }

    /** @brief A hold is taken; the first one makes the link fast and stops the idle work. */
    Decision OnHold() {
        Decision decision;
        if(holds_++ > 0)
            return decision;

        decision.idle_work = IdleWork::CANCEL;
        if(!is_fast_) {
            is_fast_ = true;
            decision.request = LinkRequest::FAST;
        }

        return decision;
    }

    /** @brief A hold is released; the last one starts the idle work. A release without a hold is ignored. */
    Decision OnRelease() {
        Decision decision;
        if(holds_ == 0)
            return decision;

        holds_--;
        if(holds_ > 0)
            return decision;

        decision.idle_work = IdleWork::SCHEDULE;
        return decision;
    }

    /** @brief A write to `smp`; makes the link fast and, unless a hold is held, restarts the idle work. */
    Decision OnSmpActivity() {
        Decision decision;
        if(holds_ > 0)
            return decision;

        decision.idle_work = IdleWork::SCHEDULE;
        if(!is_fast_) {
            is_fast_ = true;
            decision.request = LinkRequest::FAST;
        }

        return decision;
    }

    /** @brief The idle work ran; the link goes idle unless a hold is held. */
    Decision OnIdleTimeout() {
        Decision decision;
        if(holds_ > 0)
            return decision;

        is_fast_ = false;
        is_fallback_armed_ = false;
        decision.request = LinkRequest::IDLE;
        return decision;
    }

    /** @brief A central connected; the new link starts with its own parameters. */
    Decision OnConnected() {
        Decision decision;
        is_fallback_armed_ = false;

        if(holds_ > 0) {
            is_fast_ = true;
            decision.request = LinkRequest::FAST;
            decision.idle_work = IdleWork::CANCEL;
        } else {
            is_fast_ = false;
            decision.idle_work = IdleWork::SCHEDULE;
        }

        return decision;
    }

    /** @brief The central is gone. Holds stay: their owners release them. */
    Decision OnDisconnected() {
        is_fast_ = false;
        is_fallback_armed_ = false;

        Decision decision;
        decision.idle_work = IdleWork::CANCEL;
        return decision;
    }

    /**
     * @brief A fast request is about to be sent; call it before sending, so an update that answers it
     * quickly is not missed.
     */
    void OnFastRequested() {
        is_fallback_armed_ = is_fast_;
    }

    /** @brief The stack refused the fast request, so no update will answer it. */
    void OnFastRequestFailed() {
        is_fallback_armed_ = false;
    }

    /**
     * @brief The connection parameters changed.
     * @param interval_units Connection interval in units of 1.25 ms.
     * @return request_fallback once per fast request, when its answer is above 15 ms.
     */
    Decision OnParametersUpdated(uint16_t interval_units) {
        Decision decision;
        if(!is_fallback_armed_)
            return decision;

        is_fallback_armed_ = false;
        decision.request_fallback = is_fast_ && interval_units > FALLBACK_THRESHOLD_UNITS;
        return decision;
    }
};

} // namespace eerie_leap::subsys::bluetooth::access_point
