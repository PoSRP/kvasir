#ifndef KVASIR_FRAME_HPP
#define KVASIR_FRAME_HPP

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace kvasir {

static constexpr size_t FRAME_HEADER_SIZE = 4; // magic(1) + type(1) + length(2BE)
static constexpr size_t SAMPLES_PER_FRAME = 64;

size_t pack_ack(CmdType cmd, AckStatus status, std::span<uint8_t> out);

class FramePacker {
public:
    size_t pack(uint8_t ch_id, std::span<const uint16_t> samples, std::span<uint8_t> out);
    void   reset() { _seq = 0; }

private:
    uint16_t _seq{0};
};

} // namespace kvasir

#endif // KVASIR_FRAME_HPP
