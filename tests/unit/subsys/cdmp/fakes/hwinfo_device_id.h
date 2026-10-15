#pragma once

// The fixed device id of fakes/hwinfo_device_id.cpp, linked on a board without a hwinfo driver.
// While it is unavailable the fake answers like Zephyr's weak implementation (-ENOSYS), which is
// what a board without a device id does to CdmpUid::Generate().
void SetFakeHwinfoDeviceIdAvailable(bool available);
