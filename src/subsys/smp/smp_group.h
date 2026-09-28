#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "smp_group_id.h"

namespace eerie_leap::subsys::smp {

/**
 * @brief Builds an entry for zcbor_map_decode_bulk(), type-checking the decoder against the value.
 *
 * Stands in for ZCBOR_MAP_DECODE_KEY_DECODER, which does not compile as C++.
 *
 * @param key Must outlive the decode.
 */
template<typename T>
zcbor_map_decode_key_val SmpMapKey(std::string_view key, bool (*decoder)(zcbor_state_t*, T*), T* value) {
    zcbor_map_decode_key_val entry{};
    entry.key.value = reinterpret_cast<const uint8_t*>(key.data());
    entry.key.len = key.size();
    entry.decoder = reinterpret_cast<zcbor_decoder_t*>(decoder);
    entry.value_ptr = value;

    return entry;
}

/** @brief Decodes the request map into @p keys; unknown keys are skipped. */
inline bool SmpDecodeRequest(smp_streamer* ctxt, std::span<zcbor_map_decode_key_val> keys) {
    size_t decoded = 0;

    return zcbor_map_decode_bulk(ctxt->reader->zs, keys.data(), keys.size(), &decoded) == 0;
}

/** @brief Appends a group error (SMP v2 `err`, or `rc` for SMP v1 clients) to the response. */
inline int SmpAddGroupError(smp_streamer* ctxt, SmpGroupId group, uint16_t error) {
    return smp_add_cmd_err(ctxt->writer->zs, std::to_underlying(group), error) ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/**
 * @brief Registers a C++ class as an MCUmgr group.
 *
 * MCUmgr passes no context to handlers, so the handlers of @p TGroup reach its registered instance
 * through Dispatch(), and only one instance of @p TGroup can be registered. Handlers run on the
 * MCUmgr work queue.
 */
template<typename TGroup>
class SmpGroup {
private:
    static inline TGroup* instance_ = nullptr;
    static inline const SmpGroup* registered_ = nullptr;
    mgmt_group group_{};

protected:
    /** @brief Handler table entry that calls @p Handler on the registered instance. */
    template<int (TGroup::*Handler)(smp_streamer*)>
    static int Dispatch(smp_streamer* ctxt) {
        TGroup* group = instance_;

        return group != nullptr ? (group->*Handler)(ctxt) : MGMT_ERR_ENOTSUP;
    }

    /**
     * @param handlers One entry per command ID; must have static storage duration.
     * @param translate_error Maps group errors to MGMT_ERR_* codes for SMP v1 clients.
     */
    SmpGroup(SmpGroupId group_id, std::span<const mgmt_handler> handlers, smp_translate_error_fn translate_error) {
        group_.mg_handlers = handlers.data();
        group_.mg_handlers_count = static_cast<uint16_t>(handlers.size());
        group_.mg_group_id = std::to_underlying(group_id);
#if defined(CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL)
        group_.mg_translate_error = translate_error;
#else
        (void)translate_error;
#endif
    }

public:
    /** @brief Unregisters; a handler still running on the MCUmgr work queue is not waited for. */
    ~SmpGroup() { Unregister(); }

    SmpGroup(const SmpGroup&) = delete;
    SmpGroup& operator=(const SmpGroup&) = delete;
    SmpGroup(SmpGroup&&) = delete;
    SmpGroup& operator=(SmpGroup&&) = delete;

    /** @throws std::logic_error if another instance of @p TGroup is registered. */
    void Register() {
        if(registered_ == this)
            return;

        if(registered_ != nullptr)
            throw std::logic_error("Another instance of this SMP group is registered");

        instance_ = static_cast<TGroup*>(this);
        registered_ = this;
        mgmt_register_group(&group_);
    }

    void Unregister() {
        if(registered_ != this)
            return;

        mgmt_unregister_group(&group_);
        instance_ = nullptr;
        registered_ = nullptr;
    }

    [[nodiscard]] bool IsRegistered() const { return registered_ == this; }
};

} // namespace eerie_leap::subsys::smp
