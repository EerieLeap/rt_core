#include <cstddef>

#include "heap_allocator.h"

#ifdef CONFIG_SHARED_MULTI_HEAP
#include <zephyr/multi_heap/shared_multi_heap.h>
#endif

namespace eerie_leap::utilities::memory {

LOG_MODULE_REGISTER(heap_allocator_logger);

#ifdef CONFIG_SHARED_MULTI_HEAP

namespace {

// shared_multi_heap forwards straight to sys_heap, which requires caller-side
// synchronization. Every thread in the firmware allocates through this resource,
// so the free lists have to be guarded here.
k_spinlock ext_heap_lock;

} // namespace

void* ExtHeapAllocate(size_t align, size_t bytes) noexcept {
    // shared_multi_heap rejects zero-sized requests, but callers still expect a
    // distinct pointer back.
    if(bytes == 0)
        bytes = 1;

    if(align == 0)
        align = alignof(std::max_align_t);

    k_spinlock_key_t key = k_spin_lock(&ext_heap_lock);
    void* pointer = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, align, bytes);
    k_spin_unlock(&ext_heap_lock, key);

    return pointer;
}

void ExtHeapFree(void* pointer) noexcept {
    if(pointer == nullptr)
        return;

    k_spinlock_key_t key = k_spin_lock(&ext_heap_lock);
    shared_multi_heap_free(pointer);
    k_spin_unlock(&ext_heap_lock, key);
}

#else

void* ExtHeapAllocate(size_t align, size_t bytes) noexcept {
    if(bytes == 0)
        bytes = 1;

    return align > alignof(std::max_align_t)
        ? k_aligned_alloc(align, bytes)
        : k_malloc(bytes);
}

void ExtHeapFree(void* pointer) noexcept {
    k_free(pointer);
}

#endif // CONFIG_SHARED_MULTI_HEAP

} // namespace eerie_leap::utilities::memory

// #ifdef CONFIG_SHARED_MULTI_HEAP

// // Without these replacements every allocation that is not explicitly PMR-aware
// // (std::function, std::string, node-based containers, plain `new`) lands in the
// // libc arena, which is whatever is left of internal SRAM - roughly 70 KB - while
// // the 4 MB external heap stays idle.
// namespace {

// constexpr size_t kDefaultNewAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

// void* NewAllocate(std::size_t size, std::size_t align) {
//     void* pointer = eerie_leap::utilities::memory::ExtHeapAllocate(align, size);
//     if(pointer == nullptr)
//         throw std::bad_alloc();

//     return pointer;
// }

// } // namespace

// void* operator new(std::size_t size) { return NewAllocate(size, kDefaultNewAlignment); }
// void* operator new[](std::size_t size) { return NewAllocate(size, kDefaultNewAlignment); }

// void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
//     return eerie_leap::utilities::memory::ExtHeapAllocate(kDefaultNewAlignment, size);
// }

// void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
//     return eerie_leap::utilities::memory::ExtHeapAllocate(kDefaultNewAlignment, size);
// }

// void operator delete(void* pointer) noexcept { eerie_leap::utilities::memory::ExtHeapFree(pointer); }
// void operator delete[](void* pointer) noexcept { eerie_leap::utilities::memory::ExtHeapFree(pointer); }
// void operator delete(void* pointer, std::size_t) noexcept { eerie_leap::utilities::memory::ExtHeapFree(pointer); }
// void operator delete[](void* pointer, std::size_t) noexcept { eerie_leap::utilities::memory::ExtHeapFree(pointer); }

// void operator delete(void* pointer, const std::nothrow_t&) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// #ifdef __cpp_aligned_new

// void* operator new(std::size_t size, std::align_val_t align) {
//     return NewAllocate(size, static_cast<std::size_t>(align));
// }

// void* operator new[](std::size_t size, std::align_val_t align) {
//     return NewAllocate(size, static_cast<std::size_t>(align));
// }

// void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
//     return eerie_leap::utilities::memory::ExtHeapAllocate(static_cast<std::size_t>(align), size);
// }

// void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
//     return eerie_leap::utilities::memory::ExtHeapAllocate(static_cast<std::size_t>(align), size);
// }

// void operator delete(void* pointer, std::align_val_t) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete[](void* pointer, std::align_val_t) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
//     eerie_leap::utilities::memory::ExtHeapFree(pointer);
// }

// #endif // __cpp_aligned_new

// #endif // CONFIG_SHARED_MULTI_HEAP
