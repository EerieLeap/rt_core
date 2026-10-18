#include <algorithm>

#include "smp_can_framer.h"

namespace eerie_leap::subsys::smp::can {

SmpCanFramer::SmpCanFramer(std::span<const uint8_t> packet, SmpCanFrameFormat format) :
    packet_(packet),
    format_(format) {}

size_t SmpCanFramer::Encode(std::span<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame) const {
    if(IsDone())
        return 0;

    const size_t data_size = std::min(format_.GetDataSize(), packet_.size() - offset_);

    frame[0] = static_cast<uint8_t>(
        (offset_ == 0 ? SmpCanFrameFormat::START_FLAG : 0) | (sequence_ & SmpCanFrameFormat::SEQUENCE_MASK)
    );
    std::copy_n(packet_.begin() + static_cast<std::ptrdiff_t>(offset_), data_size, frame.begin() + 1);

    const size_t frame_size = format_.GetPaddedSize(data_size + 1);
    std::fill(
        frame.begin() + 1 + static_cast<std::ptrdiff_t>(data_size),
        frame.begin() + static_cast<std::ptrdiff_t>(frame_size), 0
    );

    return frame_size;
}

void SmpCanFramer::Advance() {
    if(IsDone())
        return;

    offset_ += std::min(format_.GetDataSize(), packet_.size() - offset_);
    sequence_ = (sequence_ + 1) & SmpCanFrameFormat::SEQUENCE_MASK;
}

} // namespace eerie_leap::subsys::smp::can
