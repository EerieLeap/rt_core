#pragma once

#include <memory>
#include <memory_resource>

namespace eerie_memory {

template<typename T, typename... Args>
std::shared_ptr<T> make_shared_pmr(std::pmr::memory_resource* mr, Args&&... args) {
    return std::allocate_shared<T>(std::pmr::polymorphic_allocator<T>(mr), std::forward<Args>(args)...);
}

template<typename T, typename... Args>
std::shared_ptr<T> make_shared_pmr(const std::pmr::polymorphic_allocator<>& alloc, Args&&... args) {
    return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
}

template<class T>
class PmrDeleter {
private:
    std::pmr::memory_resource* mr_;

    // NOTE: If used with base class or interface, deallocator will have incorrect
    // size and alignment, which can cause memory leaks, but tests showed that it's not a case.
    // Note that having these additional fields in the deleter will increase its size.
    // size_t size_;
    // size_t alignment_;

public:
    explicit PmrDeleter(std::pmr::memory_resource* mr = std::pmr::get_default_resource()) :
        mr_(mr) {}

    PmrDeleter(PmrDeleter const&) noexcept = default;
    PmrDeleter& operator=(PmrDeleter const&) noexcept = default;
    PmrDeleter(PmrDeleter&&) noexcept = default;
    PmrDeleter& operator=(PmrDeleter&&) noexcept = default;
    ~PmrDeleter() = default;

    template<class U>
        requires std::convertible_to<U*, T*>
    // NOLINTNEXTLINE(google-explicit-constructor): the converting deleter must be implicit so unique_ptr<Derived> converts to unique_ptr<Base>
    PmrDeleter(const PmrDeleter<U>& other) noexcept :
        mr_(other.GetMemoryResource()) {}

    template<class U>
        requires std::convertible_to<U*, T*>
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved, google-explicit-constructor): the deleter only holds a raw pointer, moving it is copying it; the converting deleter must be implicit so unique_ptr<Derived> converts to unique_ptr<Base>
    PmrDeleter(PmrDeleter<U>&& other) noexcept :
        mr_(other.GetMemoryResource()) {}

    void operator()(T* p) const noexcept {
        if(!p)
            return;

        std::destroy_at(p);
        mr_->deallocate(p, sizeof(T), alignof(T));
    }

    // Add accessor so other instantiations can access mr_
    std::pmr::memory_resource* GetMemoryResource() const noexcept {
        return mr_;
    }

    // Make all instantiations friends of each other
    template<class U>
    friend class PmrDeleter;

    // PmrDeleter(
    //     std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
    //     size_t size = sizeof(T),
    //     size_t alignment = alignof(T))
    //     : mr_(mr), size_(size), alignment_(alignment) {}

    // template<class U>
    //     requires std::convertible_to<U*, T*>
    // PmrDeleter(const PmrDeleter<U>& other) noexcept
    //     : mr_(other.mr_), size_(other.size_), alignment_(other.alignment_) {}

    // template<class U>
    //     requires std::convertible_to<U*, T*>
    // PmrDeleter(PmrDeleter<U>&& other) noexcept
    //     : mr_(other.mr_), size_(other.size_), alignment_(other.alignment_) {}

    // void operator()(T* p) const noexcept {
    //     if(!p)
    //         return;

    //     std::destroy_at(p);
    //     mr_->deallocate(p, size_, alignment_);
    // }
};

template<typename T>
using PmrUniquePtr = std::unique_ptr<T, PmrDeleter<T>>;

template<typename T, typename... Args>
PmrUniquePtr<T> make_unique_pmr(std::pmr::memory_resource* mr, Args&&... args) {
    std::pmr::polymorphic_allocator<T> prm_allocator(mr);
    T* ptr = prm_allocator.allocate(1);

    try {
        prm_allocator.construct(ptr, std::forward<Args>(args)...);
        return PmrUniquePtr<T>(ptr, PmrDeleter<T>(mr));
    } catch(...) {
        prm_allocator.deallocate(ptr, 1);
        throw;
    }
}

template<typename T, typename... Args>
PmrUniquePtr<T> make_unique_pmr(std::pmr::polymorphic_allocator<> alloc, Args&&... args) {
    return make_unique_pmr<T>(alloc.resource(), std::forward<Args>(args)...);
}

} // namespace eerie_memory
