#pragma once

#include <cstdint>
#include <memory>

#include "smp_packet.h"
#include "i_smp_response_sink.h"

namespace eerie_leap::subsys::smp {

/** @brief Sends SMP packets to other units, e.g. for the BLE router of an access point. */
class ISmpForwarder {
public:
    virtual ~ISmpForwarder() = default;

    /** @brief This unit's address on the network the packets are forwarded to; 0 while it has none. */
    [[nodiscard]] virtual uint8_t GetAddress() const = 0;

    /** @brief Whether @p target is another unit that is currently online. */
    [[nodiscard]] virtual bool IsReachable(uint8_t target) const = 0;

    /**
     * @brief Queues a packet for another unit.
     * @param target CDMP device ID of the destination.
     * @return false, with the packet freed, if it cannot be queued.
     */
    virtual bool Forward(uint8_t target, SmpPacket packet) = 0;

    /** @brief Sets where the responses to forwarded requests go; nullptr drops them. */
    virtual void SetResponseSink(std::shared_ptr<ISmpResponseSink> response_sink) = 0;
};

} // namespace eerie_leap::subsys::smp
