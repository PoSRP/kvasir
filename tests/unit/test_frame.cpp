#include <catch2/catch_test_macros.hpp>
#include "frame.hpp"
#include "protocol.hpp"
#include <array>
#include <cstring>

using namespace kvasir;

static uint16_t read_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8u) | p[1]);
}

TEST_CASE("pack_ack has correct magic type and length", "[frame]")
{
    std::array<uint8_t, 16> out{};
    pack_ack(CmdType::START, AckStatus::OK, out);
    REQUIRE(out[0] == 0xAD);
    REQUIRE(out[1] == static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(read_be16(&out[2]) == 2);
}

TEST_CASE("pack_ack encodes cmd and status", "[frame]")
{
    std::array<uint8_t, 16> out{};
    pack_ack(CmdType::STOP, AckStatus::ERROR, out);
    REQUIRE(out[4] == static_cast<uint8_t>(CmdType::STOP));
    REQUIRE(out[5] == static_cast<uint8_t>(AckStatus::ERROR));
}

TEST_CASE("stream_data has correct magic and type", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 2> samples = {1, 2};
    std::array<uint8_t, 64> out{};
    p.pack(0, samples, out);
    REQUIRE(out[0] == 0xAD);
    REQUIRE(out[1] == static_cast<uint8_t>(FrameType::STREAM_DATA));
}

TEST_CASE("stream_data length field is correct", "[frame]")
{
    FramePacker             p;
    std::array<uint16_t, 3> samples{};
    std::array<uint8_t, 64> out{};
    size_t                  total = p.pack(0, samples, out);
    uint16_t                len   = read_be16(&out[2]);
    REQUIRE(len == 1 + 2 + 2 + (3 * 2));
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
