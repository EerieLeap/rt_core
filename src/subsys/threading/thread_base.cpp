#include <algorithm>
#include <cmath>

#include "thread_base.h"

namespace eerie_leap::subsys::threading {

ThreadBase::ThreadBase(
    std::string name,
    int stack_size,
    int priority,
    bool is_cooperative,
    std::pmr::memory_resource* mr)
    : mr_(mr),
      k_stack_size_(stack_size),
      // Priority 0 is preemptive, so a cooperative thread has to settle for -1.
      k_priority_(is_cooperative
        ? -std::max(std::abs(priority), 1) : std::abs(priority)),
      stack_area_(nullptr),
      name_(std::move(name)) {}

ThreadBase::~ThreadBase() {
    if(stack_area_ == nullptr)
        return;

    if(mr_ == nullptr)
        k_thread_stack_free(stack_area_);
    else
        mr_->deallocate(stack_area_, K_KERNEL_STACK_LEN(k_stack_size_), Z_KERNEL_STACK_OBJ_ALIGN);
}

bool ThreadBase::InitializeStack() {
    if(stack_area_ != nullptr)
        return true;

    if(mr_ == nullptr)
        stack_area_ = k_thread_stack_alloc(k_stack_size_, 0);
    else
        stack_area_ = static_cast<k_thread_stack_t*>(
            mr_->allocate(K_KERNEL_STACK_LEN(k_stack_size_), Z_KERNEL_STACK_OBJ_ALIGN));

    return stack_area_ != nullptr;
}

[[nodiscard]] const k_thread_stack_t* ThreadBase::GetStack() const {
    return stack_area_;
}

} // namespace eerie_leap::subsys::threading
