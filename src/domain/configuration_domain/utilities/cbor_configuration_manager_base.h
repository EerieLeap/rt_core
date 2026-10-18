#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "configuration/services/cbor_configuration_service.h"
#include "subsys/threading/scoped_mutex.h"

#include "i_cbor_configuration_manager.h"

namespace eerie_leap::domain::configuration_domain::utilities {

// Derived classes call LoadOrCreateDefault() from their constructor, once their parser exists.
template<typename TConfiguration, typename TCborConfig>
class CborConfigurationManagerBase : public ICborConfigurationManager {
private:
    const char* name_;
    std::unique_ptr<eerie_leap::configuration::services::CborConfigurationService<TCborConfig>>
        cbor_configuration_service_;
    std::shared_ptr<TConfiguration> configuration_;
    // Get() runs on any thread (CDMP, shell, the log writer's start) while an apply replaces the
    // pointer on the configuration work queue; copying a shared_ptr that is being assigned is a race.
    mutable k_mutex configuration_mutex_{};

    // Set while the stored configuration could not be applied and an unsaved default stands in for it.
    bool is_stored_configuration_rejected_ = false;
    // Cleared only while that default is created, so the rejected file is not overwritten.
    bool is_saving_enabled_ = true;

    // Set from any thread; a store copies it under the lock and calls it outside, so a handler cleared
    // during its call finishes that call, and a call allocates nothing.
    mutable k_mutex stored_handler_mutex_{};
    std::shared_ptr<const StoredHandler> stored_handler_;

    // Both validate; Deserialize must accept everything Serialize produces.
    virtual eerie_memory::PmrUniquePtr<TCborConfig> Serialize(const TConfiguration& configuration) = 0;
    virtual eerie_memory::PmrUniquePtr<TConfiguration> Deserialize(const TCborConfig& cbor_config) = 0;
    virtual bool CreateDefaultConfiguration() = 0;
    virtual void OnConfigurationChanged(const std::shared_ptr<TConfiguration>&) {}

    // Checks a configuration a client applies against the rest of the unit before it is stored, and
    // throws std::invalid_argument with the reason to refuse it. Neither loading nor Update() calls
    // it, so a stored configuration still loads after what it was checked against has changed.
    virtual void ValidateApply(const TConfiguration&) {}

    // Takes over the parsed allocation instead of moving it into a new one.
    static std::shared_ptr<TConfiguration> Share(eerie_memory::PmrUniquePtr<TConfiguration> configuration) {
        const auto deleter = configuration.get_deleter();
        const std::pmr::polymorphic_allocator<> control_block_allocator(deleter.GetMemoryResource());

        return std::shared_ptr<TConfiguration>(configuration.release(), deleter, control_block_allocator);
    }

    std::shared_ptr<TConfiguration> CurrentConfiguration() const {
        const eerie_leap::subsys::threading::ScopedMutex guard(configuration_mutex_);

        return configuration_;
    }

    void SetConfiguration(std::shared_ptr<TConfiguration> configuration) {
        const auto current = configuration;
        {
            const eerie_leap::subsys::threading::ScopedMutex guard(configuration_mutex_);
            configuration_.swap(configuration);
        }

        // The previous configuration is released here, outside the lock, by whoever drops it last.
        OnConfigurationChanged(current);
    }

    // Serializes, parses back and saves, then adopts what was parsed.
    bool Store(const TConfiguration& configuration) {
        // NOLINTNEXTLINE(misc-const-correctness): the pointer is declared inside Zephyr's LOG_MODULE_DECLARE.
        LOG_MODULE_DECLARE(
            configuration_manager_logger
        ); // NOLINT(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro

        std::shared_ptr<TConfiguration> updated_configuration;

        try {
            auto cbor_config = Serialize(configuration);

            // Parsing the encoding back gives what a reload would, and nothing unparsable is saved.
            updated_configuration = Share(Deserialize(*cbor_config));

            if(!Save(*cbor_config))
                return false;
        } catch(const std::exception& e) {
            LOG_ERR("Failed to update %s configuration. %s", name_, e.what());
            return false;
        }

        SetConfiguration(std::move(updated_configuration));

        return true;
    }

    // After a successful store. Logs what the handler throws: the configuration is stored by then, and
    // the store must not turn into a failure a client retries.
    void NotifyStored() {
        // NOLINTNEXTLINE(misc-const-correctness): the pointer is declared inside Zephyr's LOG_MODULE_DECLARE.
        LOG_MODULE_DECLARE(
            configuration_manager_logger
        ); // NOLINT(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro

        std::shared_ptr<const StoredHandler> handler;
        {
            const eerie_leap::subsys::threading::ScopedMutex guard(stored_handler_mutex_);
            handler = stored_handler_;
        }

        if(handler == nullptr)
            return;

        try {
            (*handler)();
        } catch(const std::exception& e) {
            LOG_ERR("The %s configuration's stored handler failed. %s", name_, e.what());
        } catch(...) {
            LOG_ERR("The %s configuration's stored handler failed.", name_);
        }
    }

protected:
    CborConfigurationManagerBase(
        const char* name,
        std::unique_ptr<eerie_leap::configuration::services::CborConfigurationService<TCborConfig>>
            cbor_configuration_service
    ) :
        name_(name),
        cbor_configuration_service_(std::move(cbor_configuration_service)) {

        k_mutex_init(&configuration_mutex_);
        k_mutex_init(&stored_handler_mutex_);
    }

    // Returns whether a configuration is available afterwards.
    //
    // A stored configuration that fails to load is kept on flash: the failure may be transient
    // (a missing SD card, a changed driver) or come from a stricter validator, and saving a default
    // over it would destroy the user's configuration. A default is created in memory instead and
    // GetCborConfigurationInfo() reports the stored one as not applied.
    bool LoadOrCreateDefault() {
        // NOLINTNEXTLINE(misc-const-correctness): the pointer is declared inside Zephyr's LOG_MODULE_DECLARE.
        LOG_MODULE_DECLARE(
            configuration_manager_logger
        ); // NOLINT(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro

        try {
            if(Get(true) != nullptr) {
                LOG_INF("%s configuration loaded successfully.", name_);
                return true;
            }
        } catch(const std::exception& e) {
            LOG_ERR("Failed to load %s configuration. %s", name_, e.what());
        } catch(...) {
            LOG_ERR("Failed to load %s configuration.", name_);
        }

        // Load() records the file's size before decoding it, so a non-zero size means a file exists.
        if(cbor_configuration_service_->GetStoredInfo().size > 0) {
            LOG_ERR("Stored %s configuration is kept but not applied; running with defaults.", name_);

            is_stored_configuration_rejected_ = true;

            bool created = false;
            is_saving_enabled_ = false;
            try {
                created = CreateDefaultConfiguration();
            } catch(...) {
                is_saving_enabled_ = true;
                throw;
            }
            is_saving_enabled_ = true;

            if(!created)
                LOG_ERR("Failed to create default %s configuration.", name_);

            return created;
        }

        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default %s configuration.", name_);
            return false;
        }

        LOG_INF("Default %s configuration created successfully.", name_);

        return true;
    }

    bool Save(TCborConfig& cbor_config) {
        if(!is_saving_enabled_)
            return true;

        if(!cbor_configuration_service_->Save(&cbor_config))
            return false;

        is_stored_configuration_rejected_ = false;

        return true;
    }

    // Keeps the given instance, so it is validated only once; callers must not modify it afterwards.
    bool Adopt(std::shared_ptr<TConfiguration> configuration) {
        // NOLINTNEXTLINE(misc-const-correctness): the pointer is declared inside Zephyr's LOG_MODULE_DECLARE.
        LOG_MODULE_DECLARE(
            configuration_manager_logger
        ); // NOLINT(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro

        try {
            auto cbor_config = Serialize(*configuration);

            if(!Save(*cbor_config))
                return false;
        } catch(const std::exception& e) {
            LOG_ERR("Failed to update %s configuration. %s", name_, e.what());
            return false;
        }

        SetConfiguration(std::move(configuration));
        NotifyStored();

        return true;
    }

public:
    std::shared_ptr<TConfiguration> Get(bool force_load = false) {
        if(!force_load) {
            auto current = CurrentConfiguration();
            if(current != nullptr)
                return current;
        }

        auto loaded_config = cbor_configuration_service_->Load();
        if(!loaded_config.has_value())
            return nullptr;

        // loaded_config still owns the bytes that decoded strings point into.
        SetConfiguration(Share(Deserialize(*loaded_config->config)));

        return CurrentConfiguration();
    }

    // A configuration update: reports to the stored handler (SetStoredHandler()). Virtual, like
    // ApplyCborConfiguration(), so a manager adds its updated handler by overriding it.
    virtual bool Update(const TConfiguration& configuration) {
        if(!Store(configuration))
            return false;

        NotifyStored();

        return true;
    }

    // Stores a setting the unit keeps for itself, such as the display brightness its own UI persists:
    // as Update(), but no stored handler is called, so it is no configuration update and stops no
    // logging (docs/logging.md, "Configuration updates"). A derived manager's updated handler is not
    // called either.
    bool UpdateOwnSetting(const TConfiguration& configuration) {
        return Store(configuration);
    }

    void SetStoredHandler(StoredHandler handler) override {
        std::shared_ptr<const StoredHandler> slot;
        if(handler)
            slot = std::make_shared<const StoredHandler>(std::move(handler));

        const eerie_leap::subsys::threading::ScopedMutex guard(stored_handler_mutex_);
        stored_handler_.swap(slot);
        // The previous handler is released after the lock, by whoever drops it last.
    }

    using ICborConfigurationManager::ApplyCborConfiguration;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override {
        // NOLINTNEXTLINE(misc-const-correctness): the pointer is declared inside Zephyr's LOG_MODULE_DECLARE.
        LOG_MODULE_DECLARE(
            configuration_manager_logger
        ); // NOLINT(misc-const-correctness): the log_source_dynamic_data pointer is declared by the Zephyr macro

        const auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
        if(cbor_config == nullptr) {
            SetReason(reason, "Malformed CBOR");
            return false;
        }

        std::shared_ptr<TConfiguration> configuration;

        try {
            configuration = Share(Deserialize(*cbor_config));
            ValidateApply(*configuration);

            // Stored as received, so its CRC stays the one the sender computed.
            if(!cbor_configuration_service_->Save(cbor_data)) {
                SetReason(reason, "Could not store the configuration");
                return false;
            }
        } catch(const std::exception& e) {
            LOG_ERR("Failed to apply %s CBOR configuration. %s", name_, e.what());
            SetReason(reason, e.what());
            return false;
        }

        is_stored_configuration_rejected_ = false;
        SetConfiguration(std::move(configuration));

        LOG_INF("%s CBOR configuration applied successfully.", name_);

        NotifyStored();

        return true;
    }

    std::pmr::vector<uint8_t> GetCborConfiguration() override {
        if(Get() == nullptr)
            return {};

        auto config_bytes = cbor_configuration_service_->LoadRaw();
        if(!config_bytes.has_value())
            return {};

        return std::move(*config_bytes);
    }

    StoredCborInfo GetCborConfigurationInfo() override {
        if(CurrentConfiguration() == nullptr)
            return {};

        auto info = cbor_configuration_service_->GetStoredInfo();
        info.is_applied = !is_stored_configuration_rejected_;

        return info;
    }

    /** @return Whether the stored configuration failed to load and an unsaved default is in use. */
    [[nodiscard]] bool IsStoredConfigurationRejected() const noexcept {
        return is_stored_configuration_rejected_;
    }
};

} // namespace eerie_leap::domain::configuration_domain::utilities
