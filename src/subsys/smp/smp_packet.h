#pragma once

#include <memory>

#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>

namespace eerie_leap::subsys::smp {

/** @brief Returns a packet to the MCUmgr pool. */
struct SmpPacketDeleter {
    void operator()(net_buf* packet) const { smp_packet_free(packet); }
};

/** @brief An owned MCUmgr net_buf from the shared SMP pool. */
using SmpPacket = std::unique_ptr<net_buf, SmpPacketDeleter>;

/** @brief Takes a packet from the pool without waiting; empty when the pool is exhausted. */
inline SmpPacket AllocateSmpPacket() {
    return SmpPacket(smp_packet_alloc());
}

} // namespace eerie_leap::subsys::smp
