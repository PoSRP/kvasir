#include "frame.hpp"

namespace kvasir {

static void write_header(std::span<uint8_t> out, FrameType type, uint16_t payload_len)
{
    out[0] = 0xAD;
    out[1] = static_cast<uint8_t>(type);
    out[2] = static_cast<uint8_t>(static_cast<uint32_t>(payload_len) >> 8u);
    out[3] = static_cast<uint8_t>(payload_len);
}

size_t pack_ack(CmdType cmd, AckStatus status, std::span<uint8_t> out)
{
    constexpr uint16_t PAYLOAD_LEN = 2;
    if (out.size() < FRAME_HEADER_SIZE + PAYLOAD_LEN) {
        return 0;
    }
    write_header(out, FrameType::ACK, PAYLOAD_LEN);
    out[FRAME_HEADER_SIZE]     = static_cast<uint8_t>(cmd);
    out[FRAME_HEADER_SIZE + 1] = static_cast<uint8_t>(status);
    return FRAME_HEADER_SIZE + PAYLOAD_LEN;
}

size_t FramePacker::pack(uint8_t ch_id, std::span<const uint16_t> samples, std::span<uint8_t> out)
{
    auto count   = static_cast<uint16_t>(samples.size());
    auto payload = static_cast<uint16_t>(1 + 2 + 2 + (count * 2u));
    if (out.size() < FRAME_HEADER_SIZE + payload) {
        return 0;
    }

    write_header(out, FrameType::STREAM_DATA, payload);

    size_t i = FRAME_HEADER_SIZE;
    out[i++] = ch_id;
    out[i++] = static_cast<uint8_t>(static_cast<uint32_t>(_seq) >> 8u);
    out[i++] = static_cast<uint8_t>(_seq);
    out[i++] = static_cast<uint8_t>(static_cast<uint32_t>(count) >> 8u);
    out[i++] = static_cast<uint8_t>(count);
    ++_seq;

    for (uint16_t s : samples) {
        out[i++] = static_cast<uint8_t>(static_cast<uint32_t>(s) >> 8u);
        out[i++] = static_cast<uint8_t>(s);
    }

    return FRAME_HEADER_SIZE + payload;
}

} // namespace kvasir
