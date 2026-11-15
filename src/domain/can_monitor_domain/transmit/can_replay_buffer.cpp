#include "domain/can_monitor_domain/transmit/can_replay_buffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "domain/can_monitor_domain/utilities/can_record.h"
#include "domain/can_monitor_domain/utilities/can_record_reader.h"

namespace eerie_leap::domain::can_monitor_domain::transmit {

using utilities::CAN_MAX_RECORD_SIZE;
using utilities::CanRecord;
using utilities::CanRecordKind;
using utilities::ParseRecord;

namespace {

constexpr size_t STORAGE_ALIGNMENT = 8;
constexpr uint64_t PERCENT = 100;

} // namespace

CanReplayBuffer::CanReplayBuffer(std::pmr::memory_resource* memory, uint8_t* data, size_t capacity) :
    memory_(memory),
    data_(data),
    capacity_(capacity) {}

std::unique_ptr<CanReplayBuffer> CanReplayBuffer::Create(std::pmr::memory_resource* memory, size_t capacity) {
    if(memory == nullptr || capacity == 0)
        return nullptr;

    uint8_t* data = nullptr;
    try {
        data = static_cast<uint8_t*>(memory->allocate(capacity, STORAGE_ALIGNMENT));
    } catch(const std::bad_alloc&) {
        return nullptr;
    }

    if(data == nullptr)
        return nullptr;

    // The constructor is private, so make_unique can't reach it.
    std::unique_ptr<CanReplayBuffer> buffer(new(std::nothrow) CanReplayBuffer(memory, data, capacity));
    if(!buffer)
        memory->deallocate(data, capacity, STORAGE_ALIGNMENT);

    return buffer;
}

CanReplayBuffer::~CanReplayBuffer() {
    memory_->deallocate(data_, capacity_, STORAGE_ALIGNMENT);
}

size_t CanReplayBuffer::Wrap(size_t position) const {
    return position >= capacity_ ? position - capacity_ : position;
}

// @p offset counts from the oldest byte held; the caller keeps it within the capacity.
void CanReplayBuffer::CopyOut(size_t offset, std::span<uint8_t> out) const {
    const size_t from = Wrap(start_ + offset);
    const size_t first = std::min(out.size(), capacity_ - from);

    std::memcpy(out.data(), data_ + from, first);
    if(out.size() > first)
        std::memcpy(out.data() + first, data_, out.size() - first);
}

void CanReplayBuffer::CopyIn(size_t offset, std::span<const uint8_t> in) {
    const size_t to = Wrap(start_ + offset);
    const size_t first = std::min(in.size(), capacity_ - to);

    std::memcpy(data_ + to, in.data(), first);
    if(in.size() > first)
        std::memcpy(data_, in.data() + first, in.size() - first);
}

std::optional<CanReplayFrame> CanReplayBuffer::ReadNext(size_t& record_size, uint64_t& dt_us) const {
    if(pending_ == 0)
        return std::nullopt;

    // A record never wraps in the caller's chunk but may in the ring, so it is read out first.
    std::array<uint8_t, CAN_MAX_RECORD_SIZE> bytes{};
    const size_t count = std::min(pending_, bytes.size());
    CopyOut(retained_, std::span(bytes).first(count));

    const std::optional<CanRecord> record = ParseRecord(std::span<const uint8_t>(bytes).first(count));
    if(!record.has_value() || record->kind != CanRecordKind::FRAME)
        return std::nullopt;

    CanReplayFrame out;
    out.frame.id = record->id;
    out.frame.is_extended = record->ide;
    out.frame.is_can_fd = record->fdf;
    out.frame.is_bitrate_switch = record->brs;
    out.frame.is_remote_request = record->rtr;
    out.frame.dlc = record->dlc;
    out.frame.data.Assign(record->payload);

    record_size = record->size;
    dt_us = record->dt_us;

    return out;
}

// Works out when the frame at the front is due. Called once for each frame that comes to the front, because
// it takes the remainder of the division from the frame before.
void CanReplayBuffer::PrepareNext() {
    size_t size = 0;
    uint64_t dt_us = 0;

    if(!ReadNext(size, dt_us).has_value()) {
        // Chunks are checked on the way in, so this is storage gone wrong; stopping beats looping on it.
        pending_ = 0;
        return;
    }

    const uint64_t scaled_numerator = (std::min(dt_us, MAX_DT_US) * PERCENT) + carry_;
    carry_ = scaled_numerator % speed_pct_;
    const uint64_t scaled_us = scaled_numerator / speed_pct_;
    due_us_ = scaled_us > UINT64_MAX - base_us_ ? UINT64_MAX : base_us_ + scaled_us;
}

void CanReplayBuffer::TryRestart() {
    if(!active_ || !finished_ || !loop_ || discarded_ || pending_ != 0 || retained_ == 0)
        return;

    pending_ = retained_;
    retained_ = 0;
    carry_ = 0;
    ++loops_;

    PrepareNext();
}

bool CanReplayBuffer::Start(uint32_t speed_pct, bool loop, uint64_t now_us) {
    if(speed_pct < MIN_SPEED_PCT || speed_pct > MAX_SPEED_PCT)
        return false;

    start_ = 0;
    retained_ = 0;
    pending_ = 0;

    active_ = true;
    loop_ = loop;
    finished_ = false;
    discarded_ = false;
    speed_pct_ = speed_pct;
    next_seq_ = 0;

    base_us_ = now_us;
    carry_ = 0;
    due_us_ = now_us;

    frames_ = 0;
    late_ = 0;
    underruns_ = 0;
    loops_ = 0;

    return true;
}

CanReplayAdd CanReplayBuffer::AddChunk(uint16_t seq, std::span<const uint8_t> bytes, uint64_t now_us) {
    if(!active_ || finished_)
        return CanReplayAdd::BAD_STATE;

    if(seq != next_seq_)
        return CanReplayAdd::BAD_SEQ;

    if(bytes.size() > Room())
        return CanReplayAdd::NO_ROOM;

    for(size_t position = 0; position < bytes.size();) {
        const std::optional<CanRecord> record = ParseRecord(bytes.subspan(position));
        if(!record.has_value() || record->kind != CanRecordKind::FRAME || record->size == 0)
            return CanReplayAdd::MALFORMED;

        position += record->size;
    }

    // The consumed bytes a loop keeps give way to a chunk that needs their room: the replay did not fit.
    if(bytes.size() > capacity_ - retained_ - pending_) {
        start_ = Wrap(start_ + retained_);
        retained_ = 0;
        discarded_ = true;
    }

    const bool was_empty = pending_ == 0;

    CopyIn(retained_ + pending_, bytes);
    pending_ += bytes.size();
    ++next_seq_;

    if(was_empty && !bytes.empty()) {
        base_us_ = std::max(base_us_, now_us);
        carry_ = 0;
        PrepareNext();
    }

    return CanReplayAdd::OK;
}

void CanReplayBuffer::Finish() {
    if(!active_ || finished_)
        return;

    finished_ = true;
    TryRestart();
}

void CanReplayBuffer::Stop() {
    active_ = false;
    start_ = 0;
    retained_ = 0;
    pending_ = 0;
}

size_t CanReplayBuffer::PendingFrames() const {
    size_t frames = 0;

    for(size_t offset = 0; offset < pending_;) {
        std::array<uint8_t, CAN_MAX_RECORD_SIZE> bytes{};
        const size_t count = std::min(pending_ - offset, bytes.size());
        CopyOut(retained_ + offset, std::span(bytes).first(count));

        const std::optional<CanRecord> record = ParseRecord(std::span<const uint8_t>(bytes).first(count));
        if(!record.has_value() || record->size == 0)
            break;

        ++frames;
        offset += record->size;
    }

    return frames;
}

std::optional<uint64_t> CanReplayBuffer::NextDueUs() const {
    if(!active_ || pending_ == 0)
        return std::nullopt;

    return due_us_;
}

std::optional<CanReplayFrame> CanReplayBuffer::Peek() const {
    if(!active_)
        return std::nullopt;

    size_t size = 0;
    uint64_t dt_us = 0;

    return ReadNext(size, dt_us);
}

std::optional<CanReplayFrame> CanReplayBuffer::Pop(uint64_t now_us) {
    if(!active_ || pending_ == 0 || now_us < due_us_)
        return std::nullopt;

    size_t size = 0;
    uint64_t dt_us = 0;

    std::optional<CanReplayFrame> out = ReadNext(size, dt_us);
    if(!out.has_value()) {
        pending_ = 0;
        return std::nullopt;
    }

    out->late_us = now_us - due_us_;
    out->late = out->late_us > LATE_US;

    if(CanLoop()) {
        retained_ += size;
    } else {
        start_ = Wrap(start_ + size);
    }
    pending_ -= size;

    ++frames_;
    if(out->late)
        ++late_;

    // A late frame has made the schedule stale: the next one counts from when this one went.
    base_us_ = out->late ? now_us : due_us_;

    if(pending_ > 0) {
        PrepareNext();
    } else if(!finished_) {
        ++underruns_;
    } else {
        TryRestart();
    }

    return out;
}

} // namespace eerie_leap::domain::can_monitor_domain::transmit
