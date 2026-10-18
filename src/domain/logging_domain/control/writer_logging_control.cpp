#include <stdexcept>
#include <utility>

#include <zephyr/logging/log.h>

#include "writer_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

LOG_MODULE_REGISTER(writer_logging_control_logger);

WriterLoggingControl::WriterLoggingControl(
    std::shared_ptr<ILoggingControl> local,
    std::shared_ptr<ILoggingControl> network,
    uint32_t own_uid
) :
    local_(std::move(local)),
    network_(std::move(network)),
    own_uid_(own_uid) {

    if(local_ == nullptr)
        throw std::invalid_argument("The writer logging control needs the unit's writer.");
}

void WriterLoggingControl::RequestStart(uint32_t target_uid) {
    Route(target_uid, true);
}

void WriterLoggingControl::RequestStop(uint32_t target_uid) {
    Route(target_uid, false);
}

void WriterLoggingControl::Route(uint32_t target_uid, bool is_start) {
    const bool is_all = target_uid == ALL_UNITS;
    const bool is_own = !is_all && target_uid == own_uid_;

    if(is_all || is_own) {
        if(is_start)
            local_->RequestStart(ALL_UNITS);
        else
            local_->RequestStop(ALL_UNITS);
    }

    if(is_own)
        return;

    if(network_ == nullptr) {
        // Nothing to send to: a request for all units is served by the writer alone.
        if(!is_all)
            LOG_WRN(
                "Logging %s for uid %u is dropped: this unit has no CDMP network.", is_start ? "start" : "stop",
                static_cast<unsigned>(target_uid)
            );
        return;
    }

    if(is_start)
        network_->RequestStart(target_uid);
    else
        network_->RequestStop(target_uid);
}

bool WriterLoggingControl::HandleLoggingCommand(bool is_start) {
    const bool is_accepted = local_->HandleLoggingCommand(is_start);

    // The wanted starts of the network half are undone by a stop from another unit.
    if(!is_start && network_ != nullptr)
        network_->HandleLoggingCommand(false);

    return is_accepted;
}

bool WriterLoggingControl::IsActive() const {
    return local_->IsActive();
}

} // namespace eerie_leap::domain::logging_domain::control
