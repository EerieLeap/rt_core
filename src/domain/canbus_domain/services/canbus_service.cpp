#include <exception>
#include <vector>

#include <zephyr/logging/log.h>

#include "subsys/fs/services/fs_service_stream_buf.h"

#include "canbus_service.h"

namespace eerie_leap::domain::canbus_domain::services {

LOG_MODULE_REGISTER(canbus_service_logger);

CanbusService::CanbusService(
    std::function<const device*(uint8_t)> dt_canbus_provider,
    std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager)
        : canbus_configuration_manager_(std::move(canbus_configuration_manager)),
          dt_canbus_provider_(std::move(dt_canbus_provider)) {

    Configure();
}

void CanbusService::Configure() {
    for(const auto& [_, handler] : configuration_reset_handlers_)
        handler();

    auto canbus_configuration = canbus_configuration_manager_->Get();

    std::unordered_map<uint8_t, std::unique_ptr<Canbus>> canbuses;
    canbuses.reserve(canbus_proxies_.size());

    for(const auto& [bus_channel, canbus_proxy]: canbus_proxies_)
        canbuses.try_emplace(bus_channel, canbus_proxy->Release());
    canbus_proxies_.clear();

    for(const auto& [_, canbus]: canbuses) {
        if(canbus->GetState() != CanbusState::STOPPED)
            canbus->Stop();
    }

    // Create missing canbuses for new configurations
    for(const auto& [bus_channel, channel_configuration] : canbus_configuration->channel_configurations) {
        const auto* canbus_device = dt_canbus_provider_(bus_channel);
        if(canbus_device == nullptr) {
            LOG_ERR("CAN device for channel %d not found.", bus_channel);
            continue;
        }

        if(!canbuses.contains(bus_channel)) {
            CanbusConfig canbus_config(
                canbus_device,
                channel_configuration.type,
                channel_configuration.bitrate,
                channel_configuration.data_bitrate,
                channel_configuration.is_extended_id);

            auto new_canbus = std::make_unique<Canbus>(canbus_config);

            if(!new_canbus->Initialize()) {
                LOG_ERR("Failed to initialize CAN channel %d.", bus_channel);
                continue;
            }

            if(!new_canbus->Start()) {
                LOG_ERR("Failed to start CAN channel %d.", bus_channel);
                continue;
            }

            new_canbus->RegisterBitrateDetectedCallback([this, bus_channel](uint32_t bitrate) {
                BitrateUpdated(bus_channel, bitrate);
            });

            canbuses.try_emplace(bus_channel, std::move(new_canbus));
        }
    }

    // Configure previously stopped Canbuses
    for(const auto& [bus_channel, channel_configuration] : canbus_configuration->channel_configurations) {
        if(!canbuses.contains(bus_channel) || canbuses.at(bus_channel)->GetState() != CanbusState::STOPPED)
            continue;

        auto canbus_instance = canbuses.at(bus_channel).get();

        CanbusConfig canbus_config(
            dt_canbus_provider_(bus_channel),
            channel_configuration.type,
            channel_configuration.bitrate,
            channel_configuration.data_bitrate,
            channel_configuration.is_extended_id);

        if(!canbus_instance->Configure(canbus_config)) {
            LOG_ERR("Failed to configure CAN channel %d.", bus_channel);
            continue;
        }

        if(!canbus_instance->Start()) {
            LOG_ERR("Failed to start CAN channel %d.", bus_channel);
            continue;
        }
    }

    for(const auto& [bus_channel, channel_configuration] : canbus_configuration->channel_configurations) {
        if(!canbuses.contains(bus_channel))
            continue;

        ConfigureUserSignals(channel_configuration);
    }

    for(const auto& [bus_channel, _] : canbuses) {
        canbus_proxies_.try_emplace(
            bus_channel,
            std::make_shared<CanbusProxy>(std::move(canbuses.at(bus_channel))));
    }

    for(const auto& [_, handler] : configuration_updated_handlers_)
        handler();

    LOG_INF("CanbusService configuration updated.");
}

[[nodiscard]] std::shared_ptr<CanbusProxy> CanbusService::GetCanbus(uint8_t bus_channel) const {
    if(!canbus_proxies_.contains(bus_channel))
        return nullptr;

    return canbus_proxies_.at(bus_channel);
}

[[nodiscard]] std::shared_ptr<CanbusProxy> CanbusService::GetComCanbus() const {
    if(!canbus_configuration_manager_->Get()->com_bus_channel.has_value()) {
        LOG_WRN("COM CANBus not configured.");
        return nullptr;
    }

    auto com_canbus = GetCanbus(canbus_configuration_manager_->Get()->com_bus_channel.value());

    if(!com_canbus) {
        LOG_WRN("COM CANBus channel %d not found.",
            canbus_configuration_manager_->Get()->com_bus_channel.value());
    }

    return com_canbus;
}

const CanChannelConfiguration* CanbusService::GetChannelConfiguration(uint8_t bus_channel) const {
    auto canbus_configuration = canbus_configuration_manager_->Get();

    if(!canbus_configuration->channel_configurations.contains(bus_channel))
        return nullptr;

    return &canbus_configuration->channel_configurations.at(bus_channel);
}

void CanbusService::BitrateUpdated(uint8_t bus_channel, uint32_t bitrate) const {
    auto canbus_configuration = canbus_configuration_manager_->Get();
    bool is_bus_channel_valid = canbus_configuration->channel_configurations.contains(bus_channel);

    if(is_bus_channel_valid && canbus_configuration_manager_->Update(*canbus_configuration))
        LOG_INF("Bitrate for bus channel %d updated to %d bps.", bus_channel, bitrate);
    else
        LOG_ERR("Failed to update bitrate for bus channel %d.", bus_channel);
}

void CanbusService::ConfigureUserSignals(const CanChannelConfiguration& channel_configuration) const {
    for(const auto& message_configuration : channel_configuration.message_configurations) {
        DbcMessage* message = nullptr;

        if(channel_configuration.dbc->HasMessage(message_configuration->frame_id))
            message = channel_configuration.dbc->GetMessage(message_configuration->frame_id);
        else
            message = channel_configuration.dbc->AddMessage(
                message_configuration->frame_id,
                message_configuration->name,
                message_configuration->message_size);

        for(const auto& signal_configuration : message_configuration->signal_configurations) {
            message->AddSignal(
                signal_configuration.name,
                signal_configuration.start_bit,
                signal_configuration.size_bits,
                signal_configuration.factor,
                signal_configuration.offset,
                signal_configuration.unit);
        }
    }
}

int CanbusService::RegisterConfigurationResetHandler(CanbusServiceUpdatedHandler handler) {
    int handler_id = configuration_reset_next_handler_id_++;
    configuration_reset_handlers_.try_emplace(handler_id, std::move(handler));

    return handler_id;
}

bool CanbusService::UnregisterConfigurationResetHandler(int handler_id) {
    return configuration_reset_handlers_.erase(handler_id) > 0;
}

int CanbusService::RegisterConfigurationUpdatedHandler(CanbusServiceUpdatedHandler handler) {
    int handler_id = configuration_updated_next_handler_id_++;
    configuration_updated_handlers_.try_emplace(handler_id, std::move(handler));

    return handler_id;
}

bool CanbusService::UnregisterConfigurationUpdatedHandler(int handler_id) {
    return configuration_updated_handlers_.erase(handler_id) > 0;
}

} // namespace eerie_leap::domain::canbus_domain::services
