#pragma once

#include <cstddef>
#include <cstdint>

namespace eerie_leap::utilities::dev_tools {

// Reports any coredump currently stored in the flash-partition coredump
// backend (CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION) over the console.
//
// PrintStoredDump() is meant to be called once, early at boot.
// It reprints the SAME stored dump on every boot until Erase()
// is called - the device may reset (fault, watchdog, power-cycle) several
// times before a UART is actually attached, and the dump would otherwise be
// missed if it were only shown once, right after the fault.
//
// Output uses the same "#CD:" wire format Zephyr's own coredump "logging"
// backend/shell use (BEGIN#/hex-lines/END#), so a captured console log can
// be fed directly into zephyr/scripts/coredump/coredump_serial_log_parser.py
// to reconstruct a binary coredump for GDB
// (zephyr/scripts/coredump/coredump_gdbserver.py).
//
// No-op (nothing is printed) on any build where
// CONFIG_DEBUG_COREDUMP_BACKEND_FLASH_PARTITION isn't enabled, or when no
// dump is currently stored - safe to call unconditionally on every board
// target.
class CoredumpReporter {
private:
    static void PrintHexChunk(const uint8_t* data, size_t len);

public:
    static void PrintStoredDump();
    static void Erase();
};

} // namespace eerie_leap::utilities::dev_tools
