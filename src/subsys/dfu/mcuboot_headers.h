#pragma once

// The headers of MCUboot's bootutil, which img_mgmt.h includes, for C++.
//
// They are C headers: they use _Static_assert, which C++ spells static_assert. Include this header
// instead of img_mgmt.h and bootutil/*.h.
//
// picolibc's sys/cdefs.h defines _Static_assert for C++ itself: the shim is only made where there is
// none, and only that one is taken away again.
#ifndef _Static_assert
    #define _Static_assert static_assert
    #define EL_DFU_STATIC_ASSERT_SHIM
#endif

#include <bootutil/bootutil_public.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>

#ifdef EL_DFU_STATIC_ASSERT_SHIM
    #undef _Static_assert
    #undef EL_DFU_STATIC_ASSERT_SHIM
#endif
