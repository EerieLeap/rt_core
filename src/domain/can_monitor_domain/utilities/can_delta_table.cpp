#include "domain/can_monitor_domain/utilities/can_delta_table.h"

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA

    #include <algorithm>
    #include <bit>
    #include <cstring>
    #include <new>

    #include "domain/can_monitor_domain/utilities/can_record_writer.h"

namespace eerie_leap::domain::can_monitor_domain::utilities {

namespace {

constexpr size_t BLOCK_ALIGNMENT = 8;
constexpr uint32_t IDE_KEY_BIT = 0x80000000U;
/** @brief Knuth's multiplicative hash constant (2^32 / phi). */
constexpr uint32_t HASH_MULTIPLIER = 2654435761U;

uint32_t KeyOf(uint32_t id, bool ide) {
    return id | (ide ? IDE_KEY_BIT : 0);
}

size_t SlotsFor(size_t capacity) {
    return std::bit_ceil(capacity * 2);
}

size_t BlockSizeFor(size_t capacity) {
    return (capacity * CanDeltaTable::MAX_PAYLOAD_SIZE) + (capacity * sizeof(uint32_t))
        + (SlotsFor(capacity) * sizeof(uint16_t)) + capacity;
}

} // namespace

CanDeltaTable::CanDeltaTable(std::pmr::memory_resource* memory, void* block, size_t block_size, size_t capacity) :
    memory_(memory),
    block_(block),
    block_size_(block_size),
    capacity_(capacity),
    slots_(SlotsFor(capacity)),
    shift_(32 - static_cast<uint32_t>(std::countr_zero(slots_))) {
    // The widest first, so that each part stays aligned: payloads, keys, index, lengths.
    auto* bytes = static_cast<uint8_t*>(block);
    payloads_ = bytes;
    keys_ = reinterpret_cast<uint32_t*>(bytes + (capacity * MAX_PAYLOAD_SIZE));
    index_ = reinterpret_cast<uint16_t*>(bytes + (capacity * MAX_PAYLOAD_SIZE) + (capacity * sizeof(uint32_t)));
    lengths_ = bytes + (capacity * MAX_PAYLOAD_SIZE) + (capacity * sizeof(uint32_t)) + (slots_ * sizeof(uint16_t));

    Reset();
}

std::unique_ptr<CanDeltaTable> CanDeltaTable::Create(size_t capacity, std::pmr::memory_resource* memory) {
    if(memory == nullptr || capacity == 0 || capacity > MAX_CAPACITY)
        return nullptr;

    const size_t block_size = BlockSizeFor(capacity);

    void* block = nullptr;
    try {
        block = memory->allocate(block_size, BLOCK_ALIGNMENT);
    } catch(const std::bad_alloc&) {
        return nullptr;
    }

    if(block == nullptr)
        return nullptr;

    std::unique_ptr<CanDeltaTable> table(new(std::nothrow) CanDeltaTable(memory, block, block_size, capacity));
    if(!table)
        memory->deallocate(block, block_size, BLOCK_ALIGNMENT);

    return table;
}

CanDeltaTable::~CanDeltaTable() {
    memory_->deallocate(block_, block_size_, BLOCK_ALIGNMENT);
}

void CanDeltaTable::Reset() {
    // The index says what is in use; the entries behind it are overwritten as they are taken.
    std::memset(index_, 0, slots_ * sizeof(uint16_t));
    count_ = 0;
}

size_t CanDeltaTable::FindSlot(uint32_t key) const {
    const size_t mask = slots_ - 1;
    size_t slot = (key * HASH_MULTIPLIER) >> shift_;

    // At most half the slots are taken, so the probe ends at the key or at an empty slot.
    while(index_[slot] != 0 && keys_[index_[slot] - 1] != key)
        slot = (slot + 1) & mask;

    return slot;
}

std::span<const uint8_t> CanDeltaTable::Lookup(uint32_t id, bool ide) const {
    const uint16_t entry = index_[FindSlot(KeyOf(id, ide))];
    if(entry == 0)
        return {};

    return { payloads_ + ((entry - 1) * MAX_PAYLOAD_SIZE), lengths_[entry - 1] };
}

bool CanDeltaTable::Store(uint32_t id, bool ide, std::span<const uint8_t> payload) {
    if(payload.empty() || payload.size() > MAX_PAYLOAD_SIZE)
        return false;

    const uint32_t key = KeyOf(id, ide);
    const size_t slot = FindSlot(key);

    size_t entry = index_[slot];
    if(entry == 0) {
        if(count_ >= capacity_)
            return false;

        entry = ++count_;
        index_[slot] = static_cast<uint16_t>(entry);
        keys_[entry - 1] = key;
    }

    std::memcpy(payloads_ + ((entry - 1) * MAX_PAYLOAD_SIZE), payload.data(), payload.size());
    lengths_[entry - 1] = static_cast<uint8_t>(payload.size());

    return true;
}

size_t CanDeltaTable::Encode(std::span<uint8_t> out, uint64_t dt_us, const CanRecordFrame& frame) {
    const size_t whole_size = EncodeFrame(out, dt_us, frame);
    if(whole_size == 0)
        return 0;

    if(frame.rtr || frame.payload.empty())
        return whole_size;

    size_t size = whole_size;

    const std::span<const uint8_t> previous = Lookup(frame.id, frame.ide);
    if(previous.size() == frame.payload.size()) {
        // head, ext, dt, ID, the masks and the bytes that changed; whole has no ext byte unless a flag set it.
        size_t changed = 0;
        for(size_t i = 0; i < previous.size(); ++i)
            changed += (previous[i] != frame.payload[i]) ? 1 : 0;

        const size_t mask_count = (frame.payload.size() + 7) / 8;
        const size_t changed_size = 2 + Leb128Size(dt_us) + (frame.ide ? 4 : 2) + mask_count + changed;

        if(changed_size < whole_size) {
            size = EncodeChangedBytes(out, dt_us, frame, previous);
            if(size == 0)
                return 0;
        }
    }

    Store(frame.id, frame.ide, frame.payload);

    return size;
}

std::optional<size_t> CanDeltaDecoder::Apply(const CanRecord& record, std::span<uint8_t> payload_out) {
    switch(record.kind) {
        case CanRecordKind::GAP:
        case CanRecordKind::TAP:
            table_.Reset();
            return 0;
        case CanRecordKind::FRAME: {
            if(payload_out.size() < record.payload.size())
                return std::nullopt;

            std::copy(record.payload.begin(), record.payload.end(), payload_out.begin());
            if(!record.rtr)
                table_.Store(record.id, record.ide, record.payload);

            return record.payload.size();
        }
        case CanRecordKind::CHANGED_BYTES: {
            const size_t length = PayloadLength(record.dlc, record.fdf, false);
            const std::span<const uint8_t> previous = table_.Lookup(record.id, record.ide);
            if(previous.size() != length || payload_out.size() < length)
                return std::nullopt;

            std::copy(previous.begin(), previous.end(), payload_out.begin());

            size_t next_changed = 0;
            for(size_t i = 0; i < length; ++i) {
                if(((record.masks[i / 8] >> (i % 8)) & 1) != 0)
                    payload_out[i] = record.payload[next_changed++];
            }

            table_.Store(record.id, record.ide, payload_out.first(length));

            return length;
        }
        default:
            return 0;
    }
}

} // namespace eerie_leap::domain::can_monitor_domain::utilities

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
