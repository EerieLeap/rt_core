#include <cstring>

#include "logging_control_test_support.h"

#if !defined(CONFIG_LOG_MODE_MINIMAL)
    #include <zephyr/logging/log_backend.h>
    #include <zephyr/logging/log_core.h>
    #include <zephyr/logging/log_msg.h>
#endif

namespace logging_control_test {

namespace {

class SpinGuard {
public:
    explicit SpinGuard(k_spinlock& lock) :
        lock_(lock),
        key_(k_spin_lock(&lock)) {}
    ~SpinGuard() {
        k_spin_unlock(&lock_, key_);
    }

    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;

private:
    k_spinlock& lock_;
    k_spinlock_key_t key_;
};

} // namespace

LogCounter& LogCounter::Get() {
    static LogCounter counter;

    return counter;
}

void LogCounter::Watch(const char* source_name) {
    SpinGuard guard(lock_);

    source_name_ = source_name;
    count_ = 0;
}

size_t LogCounter::Count() {
    // Returns once the log thread has processed everything logged so far.
    log_flush();

    SpinGuard guard(lock_);

    return count_;
}

void LogCounter::Add(const char* source_name) {
    SpinGuard guard(lock_);

    if(source_name_ != nullptr && source_name != nullptr && std::strcmp(source_name, source_name_) == 0)
        count_++;
}

} // namespace logging_control_test

#if !defined(CONFIG_LOG_MODE_MINIMAL)

namespace {

void ProcessLogMessage(const log_backend* const backend, log_msg_generic* message) {
    ARG_UNUSED(backend);

    log_msg* log_message = &message->log;
    if(log_msg_get_level(log_message) > LOG_LEVEL_WRN)
        return;

    const int16_t source_id = log_msg_get_source_id(log_message);
    if(source_id < 0)
        return;

    logging_control_test::LogCounter::Get().Add(
        log_source_name_get(Z_LOG_LOCAL_DOMAIN_ID, static_cast<uint32_t>(source_id))
    );
}

void DropLogMessages(const log_backend* const backend, uint32_t count) {
    ARG_UNUSED(backend);
    ARG_UNUSED(count);
}

void PanicLog(const log_backend* const backend) {
    ARG_UNUSED(backend);
}

const log_backend_api k_log_counter_api = {
    .process = ProcessLogMessage,
    .dropped = DropLogMessages,
    .panic = PanicLog,
};

} // namespace

// Autostarted at the default level, which passes the warnings it counts.
LOG_BACKEND_DEFINE(logging_control_log_counter, k_log_counter_api, true);

#endif // !CONFIG_LOG_MODE_MINIMAL
