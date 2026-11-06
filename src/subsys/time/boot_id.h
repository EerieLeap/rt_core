#pragma once

#include <atomic>
#include <cstdint>

#include "subsys/random/rng.h"

namespace eerie_leap::subsys::time {

/**
 * Identifies this power-on: drawn from the unit's random source at the first call and the same for
 * every later one until the unit restarts; never 0. Logs carry it so that a reader tells the files of
 * two power-ons apart, and the device group's TIME reports it. Any thread; needs EERIE_LEAP_RANDOM.
 */
inline uint32_t GetBootId() {
    // Constant-initialized, so no static guard: the toolchains' single-thread model gives the guard of a
    // dynamically initialized static no lock, and a second thread inside the first initialization would
    // throw. Two first calls may both draw; the first to store wins, and both return its ID.
    static std::atomic<uint32_t> boot_id{ 0 };

    uint32_t id = boot_id.load();
    if(id != 0)
        return id;

    uint32_t drawn = 0;
    while(drawn == 0)
        drawn = eerie_leap::subsys::random::Rng::Get<uint32_t>();

    return boot_id.compare_exchange_strong(id, drawn) ? drawn : id;
}

} // namespace eerie_leap::subsys::time
