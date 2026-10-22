#include "work_queue_probe.h"

namespace eerie_leap::subsys::dfu {

WorkQueueProbe::WorkQueueProbe(const char* name, k_work_q* work_q) :
    work_q_(work_q),
    name_(name) {

    k_work_init(&item_.work, Handler);
}

WorkQueueProbe::~WorkQueueProbe() {
    k_work_sync sync{};
    k_work_cancel_sync(&item_.work, &sync);
}

void WorkQueueProbe::Handler(k_work* work) {
    auto* item = CONTAINER_OF(work, Item, work);
    item->ran.store(true, std::memory_order_release);
}

void WorkQueueProbe::Arm() {
    item_.ran.store(false, std::memory_order_relaxed);
    // Already queued from an earlier Arm() means the queue has not got to it: the submit is a no-op.
    k_work_submit_to_queue(work_q_, &item_.work);
}

bool WorkQueueProbe::Check() const {
    return item_.ran.load(std::memory_order_acquire);
}

const char* WorkQueueProbe::GetName() const {
    return name_;
}

} // namespace eerie_leap::subsys::dfu
