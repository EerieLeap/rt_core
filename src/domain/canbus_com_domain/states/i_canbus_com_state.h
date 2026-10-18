#pragma once

#include <concepts>

#include "utilities/concepts/concepts.h"

#include "canbus_com_state_data.h"
#include "canbus_com_state_type.h"

namespace eerie_leap::domain::canbus_com_domain::states {

/**
 * @brief One state type's record, the counterpart of ICanbusComCommand.
 *
 * A state type is also constructible from a record's data bytes, so that a unit decodes the records
 * of other units with it (see CanbusComState). A decoder ignores trailing bytes it does not know and
 * gives missing trailing fields their defaults.
 */
class ICanbusComState {
public:
    virtual ~ICanbusComState() = default;

    [[nodiscard]] virtual CanbusComStateType GetStateType() const = 0;
    /** @brief 1 to 5 bytes; no data would withdraw the record. */
    [[nodiscard]] virtual CanbusComStateData GetData() const = 0;
};

/** @brief A state type that encodes a record and decodes one from its data bytes. */
template<typename T>
concept CanbusComState = std::derived_from<T, ICanbusComState> && eerie_leap::utilities::concepts::SpanConstructible<T>;

} // namespace eerie_leap::domain::canbus_com_domain::states
