#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <optional>
#include <span>
#include <exception>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"

#include "configuration/cbor/cbor_serializer.h"

#include "loaded_config.hpp"

namespace eerie_leap::configuration::services {

namespace cbor = eerie_leap::configuration::cbor;

using eerie_leap::utilities::memory::Mrm;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::WorkQueueThread;

template <typename T>
class CborConfigurationService {
private:
    const std::string configuration_dir_ = "config";

    std::string configuration_name_;
    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::unique_ptr<cbor::CborSerializer<T>> serializer_;

    const std::string configuration_file_path_ = configuration_dir_ + "/" + configuration_name_ + ".cbor";

    struct SaveTask {
        k_work work;
        CborConfigurationService<T>* instance{nullptr};
        T* configuration{nullptr};
        bool result{false};
    };

    struct LoadTask {
        k_work work;
        CborConfigurationService<T>* instance{nullptr};
        std::optional<LoadedConfig<T>> result{std::nullopt};
    };

    // NOTE: When a work queue is supplied, Save and Load run on it
    // to eliminate cases when configuration is updated from some thread
    // which will require that thread to have enough stack size for the operation.
    k_mutex mutex_;
    k_work_sync work_sync_;
    SaveTask task_save_;
    LoadTask task_load_;

    bool SaveProcessor(T* configuration) {
        LOG_MODULE_DECLARE(configuration_service_logger);

        auto config_bytes = serializer_->Serialize(*configuration);

        if(config_bytes.empty()) {
            LOG_ERR("Failed to serialize configuration %s.", configuration_file_path_.c_str());
            return false;
        }

        return fs_service_->WriteFile(configuration_file_path_, config_bytes.data(), config_bytes.size());
    }

    std::optional<LoadedConfig<T>> LoadProcessor() {
        LOG_MODULE_DECLARE(configuration_service_logger);

        if(!fs_service_->Exists(configuration_file_path_)) {
            LOG_ERR("Configuration file %s does not exist.", configuration_file_path_.c_str());
            return std::nullopt;
        }

        auto file_size = fs_service_->GetFileSize(configuration_file_path_);
        if(!file_size.has_value()) {
            LOG_ERR("Failed to stat configuration file %s.", configuration_file_path_.c_str());
            return std::nullopt;
        }

        size_t buffer_size = *file_size;
        std::pmr::vector<uint8_t> buffer(buffer_size, Mrm::GetExtPmr());
        size_t out_len = 0;

        if(!fs_service_->ReadFile(configuration_file_path_, buffer.data(), buffer_size, out_len)) {
            LOG_ERR("Failed to read configuration file %s.", configuration_file_path_.c_str());
            return std::nullopt;
        }

        buffer.resize(out_len);
        auto configuration = serializer_->Deserialize(buffer);

        if(configuration == nullptr) {
            LOG_ERR("Failed to deserialize configuration %s.", configuration_file_path_.c_str());
            return std::nullopt;
        }

        uint32_t crc = crc32_ieee(buffer.data(), buffer.size());

        LoadedConfig<T> loaded_config {
            .config_raw = std::move(buffer),
            .config = std::move(configuration),
            .checksum = crc
        };

        LOG_INF("%s configuration loaded successfully.", configuration_file_path_.c_str());

        return loaded_config;
    }

    static void WorkTaskSave(k_work* work) {
        LOG_MODULE_DECLARE(configuration_service_logger);

        SaveTask* task = CONTAINER_OF(work, SaveTask, work);

        // An exception unwinding into the work queue loop would abort the system.
        try {
            task->result = task->instance->SaveProcessor(task->configuration);
        } catch(const std::exception& e) {
            LOG_ERR("Exception while saving configuration: %s", e.what());
            task->result = false;
        } catch(...) {
            LOG_ERR("Unknown exception while saving configuration.");
            task->result = false;
        }
    }

    static void WorkTaskLoad(k_work* work) {
        LOG_MODULE_DECLARE(configuration_service_logger);

        LoadTask* task = CONTAINER_OF(work, LoadTask, work);

        try {
            task->result = task->instance->LoadProcessor();
        } catch(const std::exception& e) {
            LOG_ERR("Exception while loading configuration: %s", e.what());
            task->result = std::nullopt;
        } catch(...) {
            LOG_ERR("Unknown exception while loading configuration.");
            task->result = std::nullopt;
        }
    }

public:
    CborConfigurationService(
        std::string configuration_name,
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> work_queue_thread = nullptr)
        : configuration_name_(std::move(configuration_name)),
          fs_service_(std::move(fs_service)),
          work_queue_thread_(std::move(work_queue_thread)) {

        k_mutex_init(&mutex_);

        task_save_.instance = this;
        k_work_init(&task_save_.work, WorkTaskSave);

        task_load_.instance = this;
        k_work_init(&task_load_.work, WorkTaskLoad);

        auto funcs = cbor::CborTraitRegistry::Get<T>();
        serializer_ = std::make_unique<cbor::CborSerializer<T>>();

        if(!fs_service_->Exists(configuration_dir_))
            fs_service_->CreateDirectory(configuration_dir_);
    }

    bool Save(T* configuration) {
        LOG_MODULE_DECLARE(configuration_service_logger);

        ScopedMutex lock(mutex_);

        // Delegating to a queue we are already running on would park that thread in
        // k_work_flush() waiting for work that only it can run.
        if(work_queue_thread_ == nullptr || work_queue_thread_->IsCurrentThread())
            return SaveProcessor(configuration);

        task_save_.configuration = configuration;

        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &task_save_.work) < 0) {
            LOG_ERR("Failed to submit save of configuration %s.", configuration_file_path_.c_str());
            return false;
        }

        k_work_flush(&task_save_.work, &work_sync_);

        return task_save_.result;
    }

    std::optional<LoadedConfig<T>> Load() {
        LOG_MODULE_DECLARE(configuration_service_logger);

        ScopedMutex lock(mutex_);

        if(work_queue_thread_ == nullptr || work_queue_thread_->IsCurrentThread())
            return LoadProcessor();

        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &task_load_.work) < 0) {
            LOG_ERR("Failed to submit load of configuration %s.", configuration_file_path_.c_str());
            return std::nullopt;
        }

        k_work_flush(&task_load_.work, &work_sync_);

        return std::move(task_load_.result);
    }

    eerie_memory::pmr_unique_ptr<T> Deserialize(std::span<const uint8_t> config_bytes) {
        return serializer_->Deserialize(config_bytes);
    }

    std::pmr::vector<uint8_t> Serialize(const T& configuration) {
        return serializer_->Serialize(configuration);
    }
};

} // namespace eerie_leap::configuration::services
