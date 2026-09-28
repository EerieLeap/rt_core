#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "subsys/smp/smp_header.h"
#include "subsys/smp/can/smp_can_framer.h"

namespace smp_test {

using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;
using eerie_leap::subsys::smp::can::SmpCanFramer;
using eerie_leap::subsys::smp::can::SmpCanFrameFormat;

using Frame = std::vector<uint8_t>;

// An SMP write request with a body of the given size.
inline std::vector<uint8_t> MakePacket(size_t body_size) {
    const SmpHeader header{
        .operation = SmpOperation::WRITE,
        .version = 1,
        .length = static_cast<uint16_t>(body_size),
        .group = 64,
        .sequence = 7,
        .command_id = 1,
    };

    std::vector<uint8_t> packet(SmpHeader::SIZE + body_size);
    header.Encode(std::span<uint8_t, SmpHeader::SIZE>(packet.data(), SmpHeader::SIZE));
    for(size_t i = 0; i < body_size; i++)
        packet[SmpHeader::SIZE + i] = static_cast<uint8_t>(i * 7 + 3);

    return packet;
}

inline std::vector<Frame> MakeFrames(std::span<const uint8_t> packet, SmpCanFrameFormat format = {}) {
    std::vector<Frame> frames;
    SmpCanFramer framer(packet, format);

    while(!framer.IsDone()) {
        std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame{};
        const size_t size = framer.Encode(frame);
        frames.emplace_back(frame.begin(), frame.begin() + size);
        framer.Advance();
    }

    return frames;
}

} // namespace smp_test
