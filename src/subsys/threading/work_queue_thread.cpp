#include <ranges>
#include <algorithm>
#include <zephyr/logging/log.h>

#include "work_queue_thread.h"

LOG_MODULE_REGISTER(work_queue_thread);

namespace eerie_leap::subsys::threading {

WorkQueueThread::WorkQueueThread(
    std::string name,
    int stack_size,
    int priority,
    bool is_cooperative,
    std::pmr::memory_resource* mr)
    : ThreadBase(std::move(name), stack_size, priority, is_cooperative, mr) {

    k_mutex_init(&runner_tasks_mutex_);
}

WorkQueueThread::~WorkQueueThread() {
    k_work_queue_stop(&work_q_, K_FOREVER);
}

void WorkQueueThread::IsValid() const {
    if(!initialized_)
        throw std::runtime_error("WorkQueueThread is not initialized.");
}

void WorkQueueThread::Initialize() {
    InitializeStack();

    k_work_queue_init(&work_q_);
    k_work_queue_start(&work_q_, stack_area_, k_stack_size_, k_priority_, nullptr);

    k_thread_name_set(work_q_.thread_id, name_.c_str());

    initialized_ = true;
}

void WorkQueueThread::Stop() {
    IsValid();

    k_work_queue_drain(&work_q_, true);

    int ret = k_work_queue_stop(&work_q_, K_FOREVER);
    if(ret != 0) {
        LOG_ERR("Failed to stop work queue: %d", ret);
    }
}

[[nodiscard]] k_work_q* WorkQueueThread::GetWorkQueue() {
    IsValid();

    return &work_q_;
}

void WorkQueueThread::TaskHandler(k_work* work) {
    WorkQueueTaskBase* task = CONTAINER_OF(work, WorkQueueTaskBase, work);
    auto result = task->Execute();

    if(result.reschedule)
        task->Reschedule(result.delay);
}

void WorkQueueThread::RunnerTaskHandler(k_work* work) {
    WorkQueueRunnerTask* task = CONTAINER_OF(work, WorkQueueRunnerTask, work);
    auto result = task->Execute();

    auto mutex = task->GetMutex();
    k_mutex_lock(mutex, K_FOREVER);

    auto& tasks = task->GetRunnerTasks();
    auto& completed_tasks = task->GetCompletedTasks();

    std::erase_if(completed_tasks, [](auto& completed_task) {
        return !k_work_delayable_is_pending(&completed_task->work); });

    completed_tasks.push_back(
        std::move(tasks.extract(work).mapped()));

    k_mutex_unlock(mutex);
}

void WorkQueueThread::Run(const WorkQueueRunnerTask::Handler& handler) {
    IsValid();

    k_mutex_lock(&runner_tasks_mutex_, K_FOREVER);

    auto task = std::make_unique<WorkQueueRunnerTask>(
        &work_q_,
        &sync_,
        RunnerTaskHandler,
        handler,
        &runner_tasks_mutex_,
        runner_tasks_,
        runner_completed_tasks_);
    void* work_ptr = &task->work;

    if(runner_tasks_.insert({work_ptr, std::move(task)}).second)
        runner_tasks_.at(work_ptr)->Schedule();

    k_mutex_unlock(&runner_tasks_mutex_);
}

} // namespace eerie_leap::subsys::threading
