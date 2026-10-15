#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <zephyr/drivers/hwinfo.h>

#include "hwinfo_device_id.h"

namespace {

std::atomic<bool> device_id_available{true};

} // namespace

void SetFakeHwinfoDeviceIdAvailable(bool available) {
    device_id_available.store(available);
}

// Linked only on a board without a hwinfo driver (the QEMU boards), where Zephyr's weak
// implementation answers -ENOSYS: a fixed id lets CdmpUid::Generate() take its hardware branch.
extern "C" ssize_t z_impl_hwinfo_get_device_id(uint8_t* buffer, size_t length) {
    static constexpr std::array<uint8_t, 8> kDeviceId = {0x45, 0x4C, 0x51, 0x45, 0x4D, 0x55, 0x00, 0x01};

    if(!device_id_available.load())
        return -ENOSYS;

    const size_t count = std::min(length, kDeviceId.size());
    std::copy_n(kDeviceId.begin(), count, buffer);

    return static_cast<ssize_t>(count);
}
