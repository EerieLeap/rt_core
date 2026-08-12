#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include <zephyr/ztest.h>

#include "subsys/threading/thread_base.h"

using eerie_leap::subsys::threading::ThreadBase;

namespace {

constexpr int STACK_SIZE = 1024;

// Larger than CONFIG_HEAP_MEM_POOL_SIZE, so the kernel allocator has to refuse it.
constexpr int OVERSIZED_STACK_SIZE = 4 * 1024 * 1024;

// ThreadBase keeps everything the constructor derives in protected state.
class ProbeThreadBase : public ThreadBase {
public:
    using ThreadBase::ThreadBase;

    [[nodiscard]] int Priority() const { return k_priority_; }
    [[nodiscard]] int StackSize() const { return k_stack_size_; }
    [[nodiscard]] const std::string& Name() const { return name_; }
};

class TrackingMemoryResource : public std::pmr::memory_resource {
public:
    int allocations = 0;
    int deallocations = 0;
    std::size_t allocated_bytes = 0;
    std::size_t allocated_alignment = 0;
    std::size_t deallocated_bytes = 0;
    std::size_t deallocated_alignment = 0;
    void* allocated_pointer = nullptr;
    void* deallocated_pointer = nullptr;

private:
    std::pmr::memory_resource* upstream_ = std::pmr::new_delete_resource();

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        allocated_bytes = bytes;
        allocated_alignment = alignment;
        allocated_pointer = upstream_->allocate(bytes, alignment);

        return allocated_pointer;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        deallocated_bytes = bytes;
        deallocated_alignment = alignment;
        deallocated_pointer = pointer;
        upstream_->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

} // namespace

ZTEST_SUITE(thread_base, NULL, NULL, NULL, NULL, NULL);

ZTEST(thread_base, test_constructor_keeps_name_and_stack_size) {
    ProbeThreadBase thread("worker", STACK_SIZE, 5);

    zassert_equal(thread.Name(), std::string("worker"));
    zassert_equal(thread.StackSize(), STACK_SIZE);
}

ZTEST(thread_base, test_constructor_maps_preemptive_priority_to_positive) {
    ProbeThreadBase from_positive("preemptive_positive", STACK_SIZE, 7, false);
    ProbeThreadBase from_negative("preemptive_negative", STACK_SIZE, -7, false);

    zassert_equal(from_positive.Priority(), 7);
    zassert_equal(from_negative.Priority(), 7);
}

ZTEST(thread_base, test_constructor_maps_cooperative_priority_to_negative) {
    ProbeThreadBase from_positive("cooperative_positive", STACK_SIZE, 7, true);
    ProbeThreadBase from_negative("cooperative_negative", STACK_SIZE, -7, true);

    zassert_equal(from_positive.Priority(), -7);
    zassert_equal(from_negative.Priority(), -7);
}

ZTEST(thread_base, test_constructor_keeps_priority_zero_cooperative) {
    ProbeThreadBase thread("cooperative_zero", STACK_SIZE, 0, true);

    zassert_true(thread.Priority() < 0, "priority %d is preemptive", thread.Priority());
}

ZTEST(thread_base, test_constructor_defaults_to_preemptive) {
    ProbeThreadBase thread("defaulted", STACK_SIZE, 3);

    zassert_true(thread.Priority() > 0);
}

ZTEST(thread_base, test_GetStack_is_null_before_initialization) {
    ProbeThreadBase thread("unstarted", STACK_SIZE, 5);

    zassert_is_null(thread.GetStack());
}

ZTEST(thread_base, test_InitializeStack_uses_the_kernel_allocator_by_default) {
    ProbeThreadBase thread("kernel_stack", STACK_SIZE, 5);

    zassert_true(thread.InitializeStack());
    zassert_not_null(thread.GetStack());
}

ZTEST(thread_base, test_InitializeStack_reports_an_allocation_failure) {
    ProbeThreadBase thread("oversized_stack", OVERSIZED_STACK_SIZE, 5);

    zassert_false(thread.InitializeStack());
    zassert_is_null(thread.GetStack());
}

ZTEST(thread_base, test_InitializeStack_uses_the_supplied_memory_resource) {
    TrackingMemoryResource resource;

    {
        ProbeThreadBase thread("pmr_stack", STACK_SIZE, 5, false, &resource);
        thread.InitializeStack();

        zassert_equal(resource.allocations, 1);
        zassert_equal(resource.allocated_bytes, static_cast<std::size_t>(STACK_SIZE));
        zassert_equal(resource.allocated_alignment, static_cast<std::size_t>(Z_KERNEL_STACK_OBJ_ALIGN));
        zassert_equal(thread.GetStack(), resource.allocated_pointer);
        zassert_equal(resource.deallocations, 0);
    }

    zassert_equal(resource.deallocations, 1);
    zassert_equal(resource.deallocated_pointer, resource.allocated_pointer);
    zassert_equal(resource.deallocated_bytes, static_cast<std::size_t>(STACK_SIZE));
    zassert_equal(resource.deallocated_alignment, static_cast<std::size_t>(Z_KERNEL_STACK_OBJ_ALIGN));
}

ZTEST(thread_base, test_InitializeStack_honours_the_stack_alignment) {
    ProbeThreadBase thread("aligned_stack", STACK_SIZE, 5);

    thread.InitializeStack();

    zassert_equal(reinterpret_cast<uintptr_t>(thread.GetStack()) % Z_KERNEL_STACK_OBJ_ALIGN, 0);
}

ZTEST(thread_base, test_InitializeStack_keeps_the_stack_it_already_allocated) {
    TrackingMemoryResource resource;
    ProbeThreadBase thread("reinitialized_stack", STACK_SIZE, 5, false, &resource);

    thread.InitializeStack();
    const k_thread_stack_t* stack = thread.GetStack();

    thread.InitializeStack();

    zassert_equal(resource.allocations, 1);
    zassert_equal(thread.GetStack(), stack);
}

ZTEST(thread_base, test_destructor_does_not_free_an_unallocated_stack) {
    TrackingMemoryResource resource;

    {
        ProbeThreadBase thread("never_initialized", STACK_SIZE, 5, false, &resource);
    }

    zassert_equal(resource.allocations, 0);
    zassert_equal(resource.deallocations, 0);
}

ZTEST(thread_base, test_stacks_of_separate_instances_do_not_overlap) {
    ProbeThreadBase first("first_stack", STACK_SIZE, 5);
    ProbeThreadBase second("second_stack", STACK_SIZE, 5);

    first.InitializeStack();
    second.InitializeStack();

    zassert_not_null(first.GetStack());
    zassert_not_null(second.GetStack());
    zassert_true(first.GetStack() != second.GetStack());
}
