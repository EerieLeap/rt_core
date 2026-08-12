#include <zephyr/random/random.h>

#include "rng.h"

namespace eerie_leap::subsys::random {

void Rng::Get(void* dst, size_t len, bool secure) {
    if(!secure) {
        sys_rand_get(dst, len);
        return;
    }

#ifdef CONFIG_HARDWARE_DEVICE_CS_GENERATOR
    sys_csrand_get(dst, len);
#else
    sys_rand_get(dst, len);
#endif
}

} // namespace eerie_leap::subsys::random
