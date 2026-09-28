#include <algorithm>

#include "subsys/smp/smp_header.h"

#include "smp_can_framer.h"
#include "smp_can_reassembler.h"

namespace eerie_leap::subsys::smp::can {

SmpCanReassembler::SmpCanReassembler(int64_t timeout_ms) : timeout_ms_(timeout_ms) {}

void SmpCanReassembler::Attach(std::span<uint8_t> buffer, SmpCanFrameFormat format) {
    buffer_ = buffer;
    format_ = format;
    Reset();
}

void SmpCanReassembler::Reset() {
    length_ = 0;
    packet_size_ = 0;
    next_sequence_ = 0;
    is_active_ = false;
}

bool SmpCanReassembler::IsExpired(int64_t now_ms) const {
    return is_active_ && now_ms - last_frame_ms_ > timeout_ms_;
}

bool SmpCanReassembler::IsStartFrame(std::span<const uint8_t> frame) {
    return !frame.empty() && (frame[0] & SmpCanFrameFormat::START_FLAG) != 0;
}

SmpCanReassembler::Result SmpCanReassembler::Accept(std::span<const uint8_t> frame, int64_t now_ms) {
    if(IsExpired(now_ms))
        Reset();

    if(frame.empty() || frame.size() > format_.GetFrameSize())
        return is_active_ ? Drop() : Result::IGNORED;

    const uint8_t sequence = frame[0] & SmpCanFrameFormat::SEQUENCE_MASK;

    if(IsStartFrame(frame)) {
        Reset();
        if(sequence != 0)
            return Drop();

        is_active_ = true;
    } else {
        if(!is_active_)
            return Result::IGNORED;

        if(sequence != next_sequence_)
            return Drop();
    }

    last_frame_ms_ = now_ms;
    next_sequence_ = (sequence + 1) & SmpCanFrameFormat::SEQUENCE_MASK;

    return Append(frame.subspan(1));
}

SmpCanReassembler::Result SmpCanReassembler::Drop() {
    Reset();

    return Result::DROPPED;
}

SmpCanReassembler::Result SmpCanReassembler::Append(std::span<const uint8_t> data) {
    if(data.empty())
        return Drop();

    // Padding of the last frame may reach past the buffer, so only what fits is copied.
    const size_t start = length_;
    const size_t copied = std::min(data.size(), buffer_.size() - start);
    std::copy_n(data.begin(), copied, buffer_.begin() + start);
    length_ += copied;

    if(packet_size_ == 0 && length_ >= SmpHeader::SIZE) {
        packet_size_ = SmpHeader::Parse(buffer_.first(length_))->GetPacketSize();
        if(packet_size_ > buffer_.size())
            return Drop();
    }

    // Only the last frame of a packet may be short.
    if(packet_size_ == 0 || length_ < packet_size_) {
        if(data.size() < format_.GetDataSize() || copied < data.size())
            return Drop();

        return Result::IN_PROGRESS;
    }

    const size_t used = packet_size_ - start;
    if(format_.GetPaddedSize(used + 1) != data.size() + 1)
        return Drop();

    length_ = packet_size_;
    is_active_ = false;

    return Result::COMPLETE;
}

} // namespace eerie_leap::subsys::smp::can
