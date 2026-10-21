#pragma once

// The headers of MCUboot's bootutil, which img_mgmt.h includes, for C++.
//
// They are C headers: they use _Static_assert, which C++ spells static_assert. Include this header
// instead of img_mgmt.h and bootutil/*.h.
#define _Static_assert static_assert

#include <bootutil/bootutil_public.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>

#undef _Static_assert
