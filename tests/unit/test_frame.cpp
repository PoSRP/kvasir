#include <catch2/catch_test_macros.hpp>
#include "frame.hpp"
#include "protocol.hpp"
#include "channels.hpp"
#include <array>
#include <cstring>

// ── helpers ──────────────────────────────────────────────────────────────────

static uint16_t read_be16(const uint8_t* p) { return uint16_t(p[0] << 8) | p[1]; }
static uint32_t read_be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// ── DEVICE_INFO ──────────────────────────────────────────────────────────────

TEST_CASE("device_info has correct magic and type", "[frame]")
{
    std::array<uint8_t, 256> out{};
    pack_device_info(out);
    REQUIRE(out[0] == 0xAD);
    REQUIRE(out[1] == uint8_t(FrameType::DEVICE_INFO));
}

TEST_CASE("device_info length field matches actual payload", "[frame]")
{
    std::array<uint8_t, 256> out{};
    size_t                   total = pack_device_info(out);
    uint16_t                 len   = read_be16(&out[2]);
    REQUIRE(total == FRAME_HEADER_SIZE + len);
}

TEST_CASE("device_info reports correct firmware version", "[frame]")
{
    std::array<uint8_t, 256> out{};
    pack_device_info(out);
    REQUIRE(out[4] == FW_VERSION_MAJOR);
    REQUIRE(out[5] == FW_VERSION_MINOR);
}

TEST_CASE("device_info reports correct channel count", "[frame]")
{
    std::array<uint8_t, 256> out{};
    pack_device_info(out);
    REQUIRE(out[6] == uint8_t(CHANNELS.size()));
}

TEST_CASE("device_info first channel has correct id type and name", "[frame]")
{
    std::array<uint8_t, 256> out{};
    pack_device_info(out);
    // First channel descriptor starts at byte 7
    REQUIRE(out[7] == CHANNELS[0].id);
    REQUIRE(out[8] == uint8_t(CHANNELS[0].type));
    uint8_t name_len = out[9];
    REQUIRE(name_len == CHANNELS[0].name.size());
    REQUIRE(std::memcmp(&out[10], CHANNELS[0].name.data(), name_len) == 0);
}

TEST_CASE("device_info returns 0 when buffer too small", "[frame]")
{
    std::array<uint8_t, 4> tiny{};
    REQUIRE(pack_device_info(tiny) == 0);
}

// ── ACK ──────────────────────────────────────────────────────────────────────

TEST_CASE("pack_ack has correct magic type and length", "[frame]")
{
    std::array<uint8_t, 16> out{};
    pack_ack(CmdType::START, AckStatus::OK, out);
    REQUIRE(out[0] == 0xAD);
    REQUIRE(out[1] == uint8_t(FrameType::ACK));
    REQUIRE(read_be16(&out[2]) == 2);
}

TEST_CASE("pack_ack encodes cmd and status", "[frame]")
{
    std::array<uint8_t, 16> out{};
    pack_ack(CmdType::STOP, AckStatus::ERR_BAD_STATE, out);
    REQUIRE(out[4] == uint8_t(CmdType::STOP));
    REQUIRE(out[5] == uint8_t(AckStatus::ERR_BAD_STATE));
}

TEST_CASE("pack_ack_config includes actual entries after status", "[frame]")
{
    std::array<uint8_t, 32> out{};
    // Simulate actual config for ch_id=0: sample_rate=535000
    uint8_t actual[] = {0, 4, 0, 0x08, 0x29, 0x78}; // ch_id=0, len=4, 535000 BE
    pack_ack_config(AckStatus::OK, actual, out);
    REQUIRE(out[1] == uint8_t(FrameType::ACK));
    REQUIRE(out[4] == uint8_t(CmdType::CONFIG));
    REQUIRE(out[5] == uint8_t(AckStatus::OK));
    REQUIRE(read_be16(&out[2]) == 2 + sizeof(actual));
    REQUIRE(std::memcmp(&out[6], actual, sizeof(actual)) == 0);
}

// ── STREAM_DATA ──────────────────────────────────────────────────────────────

TEST_CASE("stream_data has correct magic and type", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 2> samples = {1, 2};
    std::array<uint8_t, 64> out{};
    p.pack(0, samples, out);
    REQUIRE(out[0] == 0xAD);
    REQUIRE(out[1] == uint8_t(FrameType::STREAM_DATA));
}

TEST_CASE("stream_data length field is correct", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 3> samples{};
    std::array<uint8_t, 64> out{};
    size_t                  total = p.pack(0, samples, out);
    uint16_t                len   = read_be16(&out[2]);
    // payload = ch_id(1) + seq(2) + count(2) + 3*2
    REQUIRE(len == 1 + 2 + 2 + 3 * 2);
    REQUIRE(total == FRAME_HEADER_SIZE + len);
}

TEST_CASE("stream_data encodes channel id", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 1> samples{};
    std::array<uint8_t, 32> out{};
    p.pack(7, samples, out);
    REQUIRE(out[4] == 7);
}

TEST_CASE("stream_data first seq is 0", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 1> samples{};
    std::array<uint8_t, 32> out{};
    p.pack(0, samples, out);
    REQUIRE(read_be16(&out[5]) == 0);
}

TEST_CASE("stream_data seq increments on successive packs", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 1> samples{};
    std::array<uint8_t, 32> out{};
    p.pack(0, samples, out);
    p.pack(0, samples, out);
    REQUIRE(read_be16(&out[5]) == 1);
}

TEST_CASE("stream_data encodes sample count", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 5> samples{};
    std::array<uint8_t, 32> out{};
    p.pack(0, samples, out);
    REQUIRE(read_be16(&out[7]) == 5);
}

TEST_CASE("stream_data samples are big-endian", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 2> samples = {0x1234, 0xABCD};
    std::array<uint8_t, 32> out{};
    p.pack(0, samples, out);
    // Samples start at byte 9 (header 4 + ch_id 1 + seq 2 + count 2)
    REQUIRE(out[9] == 0x12);
    REQUIRE(out[10] == 0x34);
    REQUIRE(out[11] == 0xAB);
    REQUIRE(out[12] == 0xCD);
}

TEST_CASE("stream_data returns 0 when buffer too small", "[frame]")
{
    FramePacker              p;
    std::array<uint16_t, 10> samples{};
    std::array<uint8_t, 4>   tiny{};
    REQUIRE(p.pack(0, samples, tiny) == 0);
}

TEST_CASE("frame_packer reset restarts seq at 0", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 1> samples{};
    std::array<uint8_t, 32> out{};
    p.pack(0, samples, out);
    p.pack(0, samples, out);
    p.reset();
    p.pack(0, samples, out);
    REQUIRE(read_be16(&out[5]) == 0);
}
