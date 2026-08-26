#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>
#include <algorithm>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

namespace eerie_leap::utilities::memory {

// Zephyr's shared_multi_heap, and the sys_heap underneath it, carry no locking of
// their own, so every access to the external heap has to be serialized here.
void* ExtHeapAllocate(size_t align, size_t bytes) noexcept;
void ExtHeapFree(void* pointer) noexcept;

template <typename T>
class HeapAllocator {
private:
    static constexpr size_t kAlignment = alignof(T) > 32 ? alignof(T) : 32;

public:
    using value_type = T;

    HeapAllocator() = default;

    template <typename U>
    constexpr HeapAllocator(const HeapAllocator<U>& obj) noexcept {}

    T* allocate(size_t n, size_t align = kAlignment) {
        void* pointer = ExtHeapAllocate(align, n * sizeof(T));

        if(pointer == nullptr) {
            LOG_MODULE_DECLARE(heap_allocator_logger);
            LOG_ERR("Failed to allocate %zu bytes for %zu elements of type %s\n", n * sizeof(T), n, typeid(T).name());
            throw std::bad_alloc();
        }

        return static_cast<T*>(pointer);
    }

    void deallocate(void* p, size_t n, size_t align = 0) noexcept {
        ExtHeapFree(p);
    }

    T* reallocate(T* p, size_t old_n, size_t new_n) {
        if(p == nullptr)
            return allocate(new_n);

        T* new_p = allocate(new_n);

        size_t copy_count = (old_n < new_n) ? old_n : new_n;

        if constexpr (std::is_trivially_copyable_v<T>) {
            memcpy(new_p, p, copy_count * sizeof(T));
        } else {
            for(size_t i = 0; i < copy_count; ++i) {
                new (new_p + i) T(std::move(p[i]));
                p[i].~T();
            }

            for(size_t i = copy_count; i < old_n; ++i)
                p[i].~T();
        }

        deallocate(p, old_n);

        return new_p;
    }
};

// TODO: Remove methods bellow after EerieLeap RoundGauge project migration
// to new memory allocator utilities is complete.

template <class T, class U>
bool operator==(const HeapAllocator<T>& lhs, const HeapAllocator<U>& rhs) { return true; }

template <class T, class U>
bool operator!=(const HeapAllocator<T>& lhs, const HeapAllocator<U>& rhs) { return false; }

template <typename T, typename... Args>
std::shared_ptr<T> make_shared_ext(Args&&... args) {
    return std::allocate_shared<T>(HeapAllocator<T>(), std::forward<Args>(args)...);
}

struct ext_deleter {
    template<typename T>
    void operator()(T* p) const {
        if (p) {
            p->~T();

            HeapAllocator<T> allocator;
            allocator.deallocate(p, 1);
        }
    }
};

template<typename T>
using ext_unique_ptr = std::unique_ptr<T, ext_deleter>;

template <typename T, typename... Args>
ext_unique_ptr<T> make_unique_ext(Args&&... args) {
    HeapAllocator<T> allocator;
    auto p = allocator.allocate(1);

    try {
        new (p) T(std::forward<Args>(args)...);
        return ext_unique_ptr<T>(p, ext_deleter{});
    } catch (...) {
        allocator.deallocate(p, 1);
        throw;
    }
}

using ExtVector = std::vector<uint8_t, HeapAllocator<uint8_t>>;
using ExtString = std::basic_string<char, std::char_traits<char>, HeapAllocator<char>>;

} // namespace eerie_leap::utilities::memory
