#include "frame.hpp"
#include "channels.hpp"
#include <algorithm>

static void write_header(std::span<uint8_t> out, FrameType type, uint16_t payload_len)
{
    out[0] = 0xAD;
    out[1] = static_cast<uint8_t>(type);
    out[2] = static_cast<uint8_t>(payload_len >> 8);
    out[3] = static_cast<uint8_t>(payload_len);
}

size_t pack_device_info(std::span<uint8_t> out)
{
    uint16_t payload_len = 3;
    for (const auto& ch : CHANNELS)
        payload_len = static_cast<uint16_t>(payload_len + 3 + ch.name.size());

    if (out.size() < FRAME_HEADER_SIZE + payload_len)
        return 0;

    write_header(out, FrameType::DEVICE_INFO, payload_len);

    size_t i = FRAME_HEADER_SIZE;
    out[i++] = FW_VERSION_MAJOR;
    out[i++] = FW_VERSION_MINOR;
    out[i++] = static_cast<uint8_t>(CHANNELS.size());

    for (const auto& ch : CHANNELS) {
        out[i++] = ch.id;
        out[i++] = static_cast<uint8_t>(ch.type);
        out[i++] = static_cast<uint8_t>(ch.name.size());
        std::copy(ch.name.begin(), ch.name.end(), out.begin() + static_cast<ptrdiff_t>(i));
        i += ch.name.size();
    }

    return i;
}

size_t pack_ack(CmdType cmd, AckStatus status, std::span<uint8_t> out)
{
    constexpr uint16_t payload_len = 2;
    if (out.size() < FRAME_HEADER_SIZE + payload_len)
        return 0;
    write_header(out, FrameType::ACK, payload_len);
    out[FRAME_HEADER_SIZE]     = static_cast<uint8_t>(cmd);
    out[FRAME_HEADER_SIZE + 1] = static_cast<uint8_t>(status);
    return FRAME_HEADER_SIZE + payload_len;
}

size_t pack_ack_config(AckStatus status, std::span<const uint8_t> actual_entries,
                       std::span<uint8_t> out)
{
    uint16_t payload_len = static_cast<uint16_t>(2 + actual_entries.size());
    if (out.size() < FRAME_HEADER_SIZE + payload_len)
        return 0;
    write_header(out, FrameType::ACK, payload_len);
    out[FRAME_HEADER_SIZE]     = static_cast<uint8_t>(CmdType::CONFIG);
    out[FRAME_HEADER_SIZE + 1] = static_cast<uint8_t>(status);
    std::copy(actual_entries.begin(), actual_entries.end(), out.begin() + FRAME_HEADER_SIZE + 2);
    return FRAME_HEADER_SIZE + payload_len;
}

size_t FramePacker::pack(uint8_t ch_id, std::span<const uint16_t> samples, std::span<uint8_t> out)
{
    auto     count   = static_cast<uint16_t>(samples.size());
    uint16_t payload = static_cast<uint16_t>(1 + 2 + 2 + count * 2u);
    if (out.size() < FRAME_HEADER_SIZE + payload)
        return 0;

    write_header(out, FrameType::STREAM_DATA, payload);

    size_t i = FRAME_HEADER_SIZE;
    out[i++] = ch_id;
    out[i++] = static_cast<uint8_t>(seq_ >> 8);
    out[i++] = static_cast<uint8_t>(seq_);
    out[i++] = static_cast<uint8_t>(count >> 8);
    out[i++] = static_cast<uint8_t>(count);
    ++seq_;

    for (uint16_t s : samples) {
        out[i++] = static_cast<uint8_t>(s >> 8);
        out[i++] = static_cast<uint8_t>(s);
    }

    return FRAME_HEADER_SIZE + payload;
}
