#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "subsys/smp/smp_group.h"
#include "domain/ble_domain/services/live_data_service.h"

namespace eerie_leap::domain::ble_domain::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::domain::ble_domain::services::LiveDataService;

/** @brief SMP `live` group: subscribes the connected central to sensor values on the `live` characteristic. */
class LiveMgmtGroup : public SmpGroup<LiveMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Write `{ids: [sensor ID hash], period}` (ms): `{period, max}`. Replaces the subscription;
        /// notifications refer to a sensor by its position in `ids`.
        SUBSCRIBE = 0,
        /// Write: `{}`.
        UNSUBSCRIBE = 1,
    };

    /** @brief Group error codes, returned in the SMP v2 `err` map. */
    enum class Error : uint16_t {
        OK = 0,
        UNKNOWN = 1,
        TOO_MANY = 2,      ///< More sensors than fit into one notification; the response carries `max`.
        NOT_LISTENING = 3, ///< The central has not enabled `live` notifications.
    };

    /** @brief Sensor ID hashes of a request; more than fit are counted, not stored. */
    struct SensorIdList {
        std::array<uint32_t, LiveDataService::MAX_SENSORS> hashes{};
        size_t count = 0;
    };

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<LiveDataService> live_data_service_;
    // Only touched on the MCUmgr work queue.
    SensorIdList sensor_ids_;

    int HandleSubscribe(smp_streamer* ctxt);
    int HandleUnsubscribe(smp_streamer* ctxt);

    static int TranslateError(uint16_t error);

public:
    explicit LiveMgmtGroup(std::shared_ptr<LiveDataService> live_data_service);
    ~LiveMgmtGroup();
};

} // namespace eerie_leap::domain::ble_domain::smp
