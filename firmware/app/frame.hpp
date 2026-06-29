#pragma once
#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

static constexpr size_t FRAME_HEADER_SIZE = 4; // magic(1) + type(1) + length(2BE)
static constexpr size_t SAMPLES_PER_FRAME = 64;

// Pack DEVICE_INFO frame using the compile-time CHANNELS table.
size_t pack_device_info(std::span<uint8_t> out);

// Pack a simple two-byte ACK (cmd_type + status).
size_t pack_ack(CmdType cmd, AckStatus status, std::span<uint8_t> out);

// Pack a CONFIG ACK that appends per-channel actual config after the status byte.
// actual_entries: raw bytes in the form [ch_id(1) cfg_len(1) cfg_data(N)]...
size_t pack_ack_config(AckStatus status, std::span<const uint8_t> actual_entries,
                       std::span<uint8_t> out);

// Packs STREAM_DATA frames; owns the sequence counter for one logical stream channel.
class FramePacker {
public:
    size_t pack(uint8_t ch_id, std::span<const uint16_t> samples, std::span<uint8_t> out);
    void   reset() { seq_ = 0; }

private:
    uint16_t seq_{0};
};
