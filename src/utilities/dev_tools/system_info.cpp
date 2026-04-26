#include <malloc.h>
#include <zephyr/logging/log.h>

#include "system_info.h"
#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/sys/sys_heap.h>

namespace eerie_leap::utilities::dev_tools {

LOG_MODULE_REGISTER(dev_tools_logger);

// Set CONFIG_THREAD_ANALYZER_ISR_STACK_USAGE for ISR stack usage info
void SystemInfo::PrintThreadInfo(int cpu) {
    thread_analyzer_print(cpu);
}

static const char* stack_info_thread_name_filter = nullptr;
static void PrintStackInfoCallback(thread_analyzer_info* info) {
    if(stack_info_thread_name_filter != nullptr && strcmp(stack_info_thread_name_filter, info->name) != 0) {
        return;
    }

    // Print stack usage information
    constexpr size_t PERCENT_MULTIPLIER = 100U;
    size_t used_percent = (info->stack_used * PERCENT_MULTIPLIER) / info->stack_size;

    LOG_INF("  %-14s: Unused %zu Usage %zu / %zu (%zu %%)",
        info->name,
        info->stack_size - info->stack_used,
        info->stack_used,
        info->stack_size,
        used_percent);
}

struct ThreadSearchData {
    const char* target_name;
    const  k_thread* found_thread;
};

static void thread_search_cb(const k_thread *thread, void *user_data) {
    auto* data = static_cast<ThreadSearchData*>(user_data);
    const char* name = thread->name;

    if(name && strcmp(name, data->target_name) == 0) {
        data->found_thread = thread;
    }
};

static void PrintStackInfoWithIdCallback(thread_analyzer_info* info) {
    if(stack_info_thread_name_filter != nullptr && strcmp(stack_info_thread_name_filter, info->name) != 0)
        return;

    extern void k_thread_foreach(k_thread_user_cb_t, void *);

    ThreadSearchData search_data = { info->name, nullptr };

    k_thread_foreach(thread_search_cb, &search_data);

    constexpr size_t PERCENT_MULTIPLIER = 100U;
    size_t used_percent = (info->stack_used * PERCENT_MULTIPLIER) / info->stack_size;

    if(search_data.found_thread) {
        LOG_INF("  %-14s: TID 0x%08x Unused %zu Usage %zu / %zu (%zu %%)",
            info->name,
            reinterpret_cast<uintptr_t>(search_data.found_thread),
            info->stack_size - info->stack_used,
            info->stack_used,
            info->stack_size,
            used_percent);
    } else {
        LOG_INF("  %-14s: TID unknown   Unused %zu Usage %zu / %zu (%zu %%)",
            info->name,
            info->stack_size - info->stack_used,
            info->stack_used,
            info->stack_size,
            used_percent);
    }
}

struct ThreadIdData {
    int count;
};

static void ThreadIdCallback(const k_thread *thread, void *user_data) {
    auto* thread_data = static_cast<ThreadIdData*>(user_data);
    const char* name = thread->name;
    if(!name) {
        name = "unnamed";
    }

    LOG_INF("  Thread %d: %-20s TID 0x%08x",
        thread_data->count++, name,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread)));
}

void SystemInfo::PrintThreadIds() {
    ThreadIdData data = {0};

    LOG_INF("Thread IDs:");
    k_thread_foreach(ThreadIdCallback, &data);
}

void SystemInfo::PrintStackInfo(int cpu, const char *thread_name) {
    stack_info_thread_name_filter = thread_name;

#ifdef CONFIG_THREAD_ANALYZER
    LOG_INF("Stack analyze for threads:");
    thread_analyzer_run(PrintStackInfoCallback, cpu);
#else
    LOG_ERR("Thread analyzer is not supprted.");
#endif
}

static const char* cpu_info_thread_name_filter = nullptr;
static void PrintCpuInfoCallback(thread_analyzer_info *info) {
    if(cpu_info_thread_name_filter != nullptr && strcmp(cpu_info_thread_name_filter, info->name) != 0)
        return;

    LOG_INF("  %-14s: CPU Load: %u %%",
		info->name,
		info->utilization);
}

void SystemInfo::PrintCpuInfo(int cpu, const char *thread_name) {
    cpu_info_thread_name_filter = thread_name;

#ifdef CONFIG_THREAD_ANALYZER
    LOG_INF("CPU analyze for threads:");
    thread_analyzer_run(PrintCpuInfoCallback, cpu);
#else
    LOG_ERR("Thread analyzer is not supprted.");
#endif
}

static void PrintHeapStats(sys_heap *heap) {
    sys_memory_stats stats;
    sys_heap_runtime_stats_get(heap, &stats);

    size_t used_percent = (stats.allocated_bytes * 100U) / (stats.free_bytes + stats.allocated_bytes);

    LOG_INF("  %-14p: Unused %zu Usage %zu / %zu (%zu %%), Max Alloc %zu",
        heap,
        stats.free_bytes,
        stats.allocated_bytes,
        stats.free_bytes + stats.allocated_bytes,
        used_percent,
        stats.max_allocated_bytes);
}

void SystemInfo::PrintHeapInfo() {
    sys_heap **heap_p;

    int heaps_count = sys_heap_array_get(&heap_p);
    LOG_INF("Heap analyze, there are %zu heaps allocated at addrs:", heaps_count);

    for(int i = 0; i < heaps_count; ++i)
        PrintHeapStats(heap_p[i]);
}

} // namespace eerie_leap::utilities::dev_tools
