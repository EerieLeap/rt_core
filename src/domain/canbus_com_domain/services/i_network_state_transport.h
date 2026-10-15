#pragma once

#include <cstddef>
#include <functional>
#include <span>

#include "domain/canbus_com_domain/states/canbus_com_state_data.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/canbus_com_domain/states/i_canbus_com_state.h"

namespace eerie_leap::domain::canbus_com_domain::services {

using eerie_leap::domain::canbus_com_domain::states::CanbusComStateData;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::canbus_com_domain::states::ICanbusComState;
using eerie_leap::domain::canbus_com_domain::states::RemoteStateRecord;

/**
 * @brief Shares this unit's state records with the other units on the network, and reads theirs.
 *
 * A unit holds at most one record per state type, and every other unit keeps a copy of its latest
 * value until the unit withdraws it or leaves. This unit's records outlive restarts of the network
 * (a CAN reconfiguration), and the other units' copies are dropped while this unit is not online.
 */
class INetworkStateTransport {
public:
    /** @brief Called with a state type whose records of other units changed. */
    using StateHandler = std::function<void(CanbusComStateType state_type)>;

    /**
     * @brief The most records of one state type GetRemoteStates() returns.
     *
     * Another unit holds at most one record per type, and this unit keeps no more copies than this in
     * all, so a span this long takes every unit's record.
     */
    static constexpr size_t MAX_REMOTE_RECORDS = CONFIG_EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS;

    virtual ~INetworkStateTransport() = default;

    /**
     * @brief Shares @p data as this unit's record of @p state_type, from any thread.
     *
     * A new value is sent while the unit is online, and otherwise on its next ONLINE. Setting the
     * current value again sends nothing, and empty data withdraws the record.
     *
     * @return False when every record slot holds another state type.
     */
    virtual bool SetLocalState(CanbusComStateType state_type, const CanbusComStateData& data) = 0;

    /** @brief Shares @p state as this unit's record of its state type, from any thread. */
    bool SetLocalState(const ICanbusComState& state) {
        return SetLocalState(state.GetStateType(), state.GetData());
    }

    /** @brief Withdraws this unit's record of @p state_type, from any thread. */
    virtual void ClearLocalState(CanbusComStateType state_type) = 0;

    /**
     * @brief Reports changes of other units' records of @p state_type.
     *
     * The handler runs on the CDMP work queue, once right after registration so that a late
     * subscriber catches up, and then whenever a record of that type is added, changed or removed,
     * which includes every copy being dropped when this unit goes offline. It must not block (publish
     * with PublishAsync), and must not register or unregister handlers.
     *
     * @return A handler ID for UnregisterStateHandler().
     */
    virtual int RegisterStateHandler(CanbusComStateType state_type, StateHandler handler) = 0;
    /** @brief Once this returns, the handler is not running and is not called again; not from a handler. */
    virtual void UnregisterStateHandler(int handler_id) = 0;

    /**
     * @brief Copies other units' records of @p state_type, ordered by device ID, from any thread.
     *
     * The copy is taken at one instant, so a handler that reads it sees at least the change it is
     * called for. A unit that is restarting keeps its records until it sends new ones, and a record
     * of it that its owner has not confirmed since is marked `is_stale`.
     *
     * A span shorter than MAX_REMOTE_RECORDS takes the records of the lowest device IDs and gives no
     * sign of the rest, so code that combines every unit's record passes one of MAX_REMOTE_RECORDS.
     *
     * @return The number of records copied, at most @p records.size().
     */
    virtual size_t GetRemoteStates(CanbusComStateType state_type, std::span<RemoteStateRecord> records) const = 0;
};

} // namespace eerie_leap::domain::canbus_com_domain::services
