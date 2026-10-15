#pragma once

#include "subsys/cdmp/models/cdmp_state_data.h"

namespace eerie_leap::domain::canbus_com_domain::states {

/**
 * @brief The data bytes of a state record: at most 5, stored without allocation.
 *
 * Empty data withdraws the record. Constructing it from more than 5 bytes throws std::invalid_argument.
 */
using CanbusComStateData = eerie_leap::subsys::cdmp::models::CdmpStateData;

/** @brief Another unit's record of one state type: `{device_id, data, is_stale}`. */
using RemoteStateRecord = eerie_leap::subsys::cdmp::models::CdmpRemoteState;

} // namespace eerie_leap::domain::canbus_com_domain::states
