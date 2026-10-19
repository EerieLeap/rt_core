#include <algorithm>
#include <charconv>
#include <exception>
#include <string>
#include <utility>

#include <eerie_memory.hpp>
#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

#include "log_end_times.h"
#include "log_file_naming.h"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

LOG_MODULE_REGISTER(log_end_times_logger);

LogEndTimes::LogEndTimes(std::shared_ptr<IFsService> fs_service) :
    fs_service_(std::move(fs_service)),
    entries_(Mrm::GetExtPmr()) {

    k_mutex_init(&mutex_);
    // Never grows past this, so Put() allocates nothing.
    entries_.reserve(MAX_ENTRIES);
}

std::string LogEndTimes::FilePath() {
    return std::string(LogFileNaming::LOGS_DIR) + "/" + LogFileNaming::END_TIMES_FILE_NAME;
}

void LogEndTimes::Put(std::string_view name, int64_t end_ms) {
    ScopedMutex lock(mutex_);

    const auto existing = std::ranges::find_if(entries_, [name](const Entry& entry) {
        return entry.name.ToString() == name;
    });
    if(existing != entries_.end())
        entries_.erase(existing);

    if(entries_.size() >= MAX_ENTRIES)
        entries_.erase(entries_.begin());

    entries_.push_back({ .name = StaticString<MAX_NAME_LENGTH>(name), .end_ms = end_ms });
}

bool LogEndTimes::Rewrite() {
    std::string content;
    {
        ScopedMutex lock(mutex_);

        for(const auto& entry : entries_) {
            content += std::to_string(entry.end_ms);
            content += ' ';
            content += entry.name.ToString();
            content += '\n';
        }
        file_lines_ = entries_.size();
    }

    return fs_service_->WriteFile(FilePath(), content.data(), content.size(), false);
}

void LogEndTimes::Load() {
    try {
        const auto path = FilePath();
        if(!fs_service_->IsAvailable())
            return;

        const uint32_t generation = fs_service_->GetMountGeneration();
        if(loaded_generation_ == generation)
            return;

        loaded_generation_ = generation;
        file_lines_ = 0;
        {
            ScopedMutex lock(mutex_);
            entries_.clear();
        }

        if(!fs_service_->Exists(path))
            return;

        const auto size = fs_service_->GetFileSize(path);
        if(!size.has_value() || *size > MAX_FILE_SIZE) {
            LOG_WRN("The log end times are unreadable; starting empty.");
            Rewrite();
            return;
        }

        std::string content(*size, '\0');
        size_t read = 0;
        if(*size > 0 && !fs_service_->ReadFile(path, content.data(), content.size(), read)) {
            LOG_WRN("Failed to read the log end times; starting empty.");
            Rewrite();
            return;
        }
        content.resize(read);

        size_t lines = 0;
        size_t kept = 0;
        bool is_dirty = false;

        std::string_view rest = content;
        while(!rest.empty()) {
            const size_t end = rest.find('\n');
            const std::string_view line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            if(line.empty())
                continue;

            lines++;

            int64_t end_ms = 0;
            const auto [after_number, error] = std::from_chars(line.data(), line.data() + line.size(), end_ms);
            const size_t name_start = static_cast<size_t>(after_number - line.data()) + 1;
            if(error != std::errc{} || after_number == line.data() + line.size() || *after_number != ' '
                || name_start >= line.size() || line.size() - name_start > MAX_NAME_LENGTH) {
                is_dirty = true;
                continue;
            }

            const auto name = line.substr(name_start);
            if(!fs_service_->Exists(std::string(LogFileNaming::LOGS_DIR) + "/" + std::string(name))) {
                is_dirty = true;
                continue;
            }

            Put(name, end_ms);
            kept++;
        }

        if(is_dirty)
            LOG_WRN("Some log end times were unreadable or belong to deleted logs; they are dropped.");

        // Also when the cap dropped entries.
        if(is_dirty || kept != Size() || lines > MAX_ENTRIES) {
            Rewrite();
        } else {
            file_lines_ = lines;
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to load the log end times: %s", e.what());
    }
}

void LogEndTimes::Record(std::string_view name, int64_t end_ms) {
    if(name.empty() || name.size() > MAX_NAME_LENGTH) {
        LOG_WRN("A log file name that does not fit the end times is not recorded.");
        return;
    }

    try {
        Put(name, end_ms);
        file_lines_++;

        if(file_lines_ > 2 * MAX_ENTRIES) {
            if(!Rewrite())
                LOG_ERR("Failed to rewrite the log end times file.");
            return;
        }

        std::string line = std::to_string(end_ms);
        line += ' ';
        line += name;
        line += '\n';

        if(!fs_service_->WriteFile(FilePath(), line.data(), line.size(), true))
            LOG_ERR("Failed to record the end of %.*s on the volume.", static_cast<int>(name.size()), name.data());
    } catch(const std::exception& e) {
        LOG_ERR("Failed to record a log end time: %s", e.what());
    }
}

std::optional<int64_t> LogEndTimes::Find(std::string_view name) const {
    ScopedMutex lock(mutex_);

    for(const auto& entry : entries_) {
        if(entry.name.ToString() == name)
            return entry.end_ms;
    }

    return std::nullopt;
}

size_t LogEndTimes::Size() const {
    ScopedMutex lock(mutex_);

    return entries_.size();
}

} // namespace eerie_leap::domain::logging_domain::services
