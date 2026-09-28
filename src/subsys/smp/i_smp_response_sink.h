#pragma once

#include <cstdint>

#include "smp_packet.h"

namespace eerie_leap::subsys::smp {

/** @brief Receives SMP responses addressed to this unit, i.e. answers to forwarded requests. */
class ISmpResponseSink {
public:
    virtual ~ISmpResponseSink() = default;

    /**
     * @brief Takes ownership of a response.
     *
     * Called from the transport's receive thread, so it must not block.
     *
     * @param source CDMP device ID of the unit that sent the response.
     */
    virtual void OnResponse(uint8_t source, SmpPacket packet) = 0;
};

} // namespace eerie_leap::subsys::smp
