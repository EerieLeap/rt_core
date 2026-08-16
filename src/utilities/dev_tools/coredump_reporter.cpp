#include <errno.h>

#include <zephyr/sys/printk.h>
#include <zephyr/debug/coredump.h>

#include "coredump_reporter.h"

namespace eerie_leap::utilities::dev_tools {

namespace {

constexpr size_t kCopyChunkSize = 128;
constexpr size_t kBytesPerLine = 32;

} // namespace

void CoredumpReporter::PrintHexChunk(const uint8_t* data, size_t len) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    char line[(kBytesPerLine * 2) + 1];

    for(size_t offset = 0; offset < len; offset += kBytesPerLine) {
        size_t line_bytes = (len - offset < kBytesPerLine) ? (len - offset) : kBytesPerLine;
        size_t pos = 0;

        for(size_t i = 0; i < line_bytes; i++) {
            uint8_t byte = data[offset + i];
            line[pos++] = kHexDigits[(byte >> 4) & 0xF];
            line[pos++] = kHexDigits[byte & 0xF];
        }
        line[pos] = '\0';

        printk("#CD:%s\n", line);
    }
}

void CoredumpReporter::PrintStoredDump() {
    if(coredump_query(COREDUMP_QUERY_HAS_STORED_DUMP, nullptr) != 1)
        return;

    int size = coredump_query(COREDUMP_QUERY_GET_STORED_DUMP_SIZE, nullptr);
    if(size <= 0)
        return;

    // NOTE: printk() is used deliberately instead of LOG_* so this doesn't
    // depend on CONFIG_LOG_MODE_DEFERRED reliability/log-buffer pressure -
    // see repo memory (freeze_debugging.md) for the prior deferred-logging
    // dropout issue found in this project.
    printk("\n#CD:BEGIN#\n");

    uint8_t buf[kCopyChunkSize];
    coredump_cmd_copy_arg copy{
        .offset = 0,
        .buffer = buf,
        .length = 0,
    };

    size_t remaining = static_cast<size_t>(size);
    while(remaining > 0) {
        copy.length = (remaining < kCopyChunkSize) ? remaining : kCopyChunkSize;

        int copied = coredump_cmd(COREDUMP_CMD_COPY_STORED_DUMP, &copy);
        if(copied <= 0) {
            printk("#CD:ERROR CANNOT DUMP#\n");
            return;
        }

        PrintHexChunk(buf, copy.length);

        copy.offset += static_cast<off_t>(copy.length);
        remaining -= copy.length;
    }

    if(coredump_query(COREDUMP_QUERY_GET_ERROR, nullptr) != 0)
        printk("#CD:ERROR CANNOT DUMP#\n");

    printk("#CD:END#\n\n");
    printk("[coredump] %d byte stored dump reported above (kept - call "
        "CoredumpReporter::Erase() once retrieved).\n", size);
}

void CoredumpReporter::Erase() {
    coredump_cmd(COREDUMP_CMD_ERASE_STORED_DUMP, nullptr);
}

} // namespace eerie_leap::utilities::dev_tools
