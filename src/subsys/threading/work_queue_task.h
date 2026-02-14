#pragma once

#include <map>
#include <functional>
#include <memory>

#include <zephyr/kernel.h>

#include "work_queue_task_result.h"

namespace eerie_leap::subsys::threading {

// Type-erased base class
class WorkQueueTaskBase {
private:
    k_work_q* work_q_;
    k_work_sync* sync_;

public:
    // NOTE: Has to be public, in order to be able to retrieve
    // an object containing that field
    k_work_delayable work;

    WorkQueueTaskBase(k_work_q* work_q, k_work_sync* sync, k_work_handler_t handler)
        : work_q_(work_q), sync_(sync) {

        k_work_init_delayable(&work, handler);
    }

    virtual ~WorkQueueTaskBase() = default;

    void Schedule(k_timeout_t delay = K_NO_WAIT) {
        k_work_schedule_for_queue(work_q_, &work, delay);
    }

    void Reschedule(k_timeout_t delay = K_NO_WAIT) {
        k_work_reschedule_for_queue(work_q_, &work, delay);
    }

    bool IsScheduled() {
        return k_work_delayable_busy_get(&work) && K_WORK_QUEUED;
    }

    bool Cancel() {
        return k_work_cancel_delayable_sync(&work, sync_);
    }

    bool Flush() {
        return k_work_flush_delayable(&work, sync_);
    }

    virtual WorkQueueTaskResult Execute() = 0;
};

// Templated derived class that stores the actual type
template<typename T>
class WorkQueueTask : public WorkQueueTaskBase {
public:
    using Handler = std::function<WorkQueueTaskResult(T*)>;

private:
    std::unique_ptr<T> user_data_;
    T* user_data_ptr_;
    Handler user_handler_;

public:
    WorkQueueTask() = default;

    WorkQueueTask(
        k_work_q* work_q,
        k_work_sync* sync,
        k_work_handler_t handler,
        const Handler& user_handler)
            : WorkQueueTaskBase(work_q, sync, handler),
            user_handler_(user_handler) {}

    WorkQueueTask(const WorkQueueTask&) = delete;
    WorkQueueTask(WorkQueueTask&&) = default;

    WorkQueueTask& operator=(const WorkQueueTask&) = delete;
    WorkQueueTask& operator=(WorkQueueTask&&) = default;

    void SetUserData(T* user_data) {
        user_data_ptr_ = user_data;
    }

    void SetUserData(std::unique_ptr<T> user_data) {
        user_data_ = std::move(user_data);
        SetUserData(user_data_.get());
    }

    T* GetUserdata() const {
        return user_data_ptr_;
    }

    WorkQueueTaskResult Execute() override {
        return user_handler_(user_data_ptr_);
    }
};

class WorkQueueRunnerTask : public WorkQueueTaskBase {
public:
    using Handler = std::function<void()>;

private:
    k_mutex* runner_tasks_mutex_;
    Handler user_handler_;
    std::map<void*, std::unique_ptr<WorkQueueRunnerTask>>& runner_tasks_;
    std::vector<std::unique_ptr<WorkQueueRunnerTask>>& completed_tasks_;

public:

    WorkQueueRunnerTask(
        k_work_q* work_q,
        k_work_sync* sync,
        k_work_handler_t handler,
        const Handler& user_handler,
        k_mutex* runner_tasks_mutex,
        std::map<void*, std::unique_ptr<WorkQueueRunnerTask>>& runner_tasks,
        std::vector<std::unique_ptr<WorkQueueRunnerTask>>& completed_tasks)
            : WorkQueueTaskBase(work_q, sync, handler),
            user_handler_(user_handler),
            runner_tasks_mutex_(runner_tasks_mutex),
            runner_tasks_(runner_tasks),
            completed_tasks_(completed_tasks) {}

    WorkQueueTaskResult Execute() override {
        user_handler_();

        return {
            .reschedule = false
        };
    }

    std::map<void*, std::unique_ptr<WorkQueueRunnerTask>>& GetRunnerTasks() {
        return runner_tasks_;
    }

    std::vector<std::unique_ptr<WorkQueueRunnerTask>>& GetCompletedTasks() {
        return completed_tasks_;
    }

    k_mutex* GetMutex() {
        return runner_tasks_mutex_;
    }
};

} // namespace eerie_leap::subsys::threading
