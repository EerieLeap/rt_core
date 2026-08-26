#include <ranges>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <zephyr/logging/log.h>

#include "scoped_mutex.h"
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
    Stop();
}

void WorkQueueThread::IsValid() const {
    if(!is_running_)
        throw std::runtime_error("WorkQueueThread is not running.");
}

bool WorkQueueThread::Initialize() {
    if(is_running_)
        return true;

    if(!InitializeStack()) {
        LOG_ERR("Failed to allocate a %d byte stack for work queue %s.", k_stack_size_, name_.c_str());
        return false;
    }

    k_work_queue_init(&work_q_);
    k_work_queue_start(&work_q_, stack_area_, k_stack_size_, k_priority_, nullptr);

    k_thread_name_set(work_q_.thread_id, name_.c_str());

    is_running_ = true;

    return true;
}

void WorkQueueThread::Stop() {
    if(!is_running_)
        return;

    // Rejects new submissions while the queue is draining.
    is_running_ = false;

    k_work_queue_drain(&work_q_, true);

    int ret = k_work_queue_stop(&work_q_, K_FOREVER);
    if(ret != 0) {
        LOG_ERR("Failed to stop work queue: %d", ret);
    }

    ScopedMutex guard(runner_tasks_mutex_);
    runner_tasks_.clear();
    runner_completed_tasks_.clear();
}

[[nodiscard]] k_work_q* WorkQueueThread::GetWorkQueue() {
    IsValid();

    return &work_q_;
}

[[nodiscard]] bool WorkQueueThread::IsCurrentThread() {
    return is_running_ && k_current_get() == k_work_queue_thread_get(&work_q_);
}

void WorkQueueThread::PruneCompletedTasks(std::vector<std::unique_ptr<WorkQueueRunnerTask>>& completed_tasks) {
    std::erase_if(completed_tasks, [](auto& completed_task) {
        return !k_work_delayable_is_pending(&completed_task->work); });
}

void WorkQueueThread::TaskHandler(k_work* work) {
    WorkQueueTaskBase* task = CONTAINER_OF(work, WorkQueueTaskBase, work);

    // Unwinding into the C work queue loop terminates the queue thread and strands
    // every other task it owns, so a throwing task must not take the queue with it.
    WorkQueueTaskResult result = task->GetLastResult();

    try {
        result = task->Execute();
        task->SetLastResult(result);
    } catch(const std::exception& e) {
        LOG_ERR("Work queue task threw: %s", e.what());
    } catch(...) {
        LOG_ERR("Work queue task threw a non-standard exception.");
    }

    if(result.reschedule)
        task->Reschedule(result.delay);
}

void WorkQueueThread::RunnerTaskHandler(k_work* work) {
    WorkQueueRunnerTask* task = CONTAINER_OF(work, WorkQueueRunnerTask, work);

    try {
        task->Execute();
    } catch(const std::exception& e) {
        LOG_ERR("Work queue runner task threw: %s", e.what());
    } catch(...) {
        LOG_ERR("Work queue runner task threw a non-standard exception.");
    }

    ScopedMutex guard(*task->GetMutex());

    auto& tasks = task->GetRunnerTasks();
    auto& completed_tasks = task->GetCompletedTasks();

    PruneCompletedTasks(completed_tasks);

    completed_tasks.push_back(
        std::move(tasks.extract(work).mapped()));
}

void WorkQueueThread::Run(const WorkQueueRunnerTask::Handler& handler) {
    IsValid();

    ScopedMutex guard(runner_tasks_mutex_);

    PruneCompletedTasks(runner_completed_tasks_);

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
}

} // namespace eerie_leap::subsys::threading
