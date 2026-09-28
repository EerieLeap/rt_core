#pragma once

#include <cstdint>

#include "smp_packet.h"

namespace eerie_leap::subsys::smp {

/** @brief Sends SMP packets to other units, e.g. for the BLE router of an access point. */
class ISmpForwarder {
public:
    virtual ~ISmpForwarder() = default;

    /**
     * @brief Queues a packet for another unit.
     * @param target CDMP device ID of the destination.
     * @return false, with the packet freed, if it cannot be queued.
     */
    virtual bool Forward(uint8_t target, SmpPacket packet) = 0;
};

} // namespace eerie_leap::subsys::smp
