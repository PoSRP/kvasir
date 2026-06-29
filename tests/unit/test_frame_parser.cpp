#include <catch2/catch_test_macros.hpp>
#include "frame_parser.hpp"
#include <vector>

// Builds a valid STREAM_DATA frame:
// [0xAD][0x01][LEN_HI][LEN_LO][ch_id][seq_hi][seq_lo][count_hi][count_lo][samples...]
static std::vector<uint8_t> make_stream_frame(uint8_t ch_id, uint16_t seq,
                                              std::vector<uint16_t> samples)
{
    uint16_t             payload_len = uint16_t(5 + samples.size() * 2);
    std::vector<uint8_t> buf;
    buf.push_back(0xAD);
    buf.push_back(0x01);
    buf.push_back(uint8_t(payload_len >> 8));
    buf.push_back(uint8_t(payload_len));
    buf.push_back(ch_id);
    buf.push_back(uint8_t(seq >> 8));
    buf.push_back(uint8_t(seq));
    buf.push_back(uint8_t(samples.size() >> 8));
    buf.push_back(uint8_t(samples.size()));
    for (uint16_t s : samples) {
        buf.push_back(uint8_t(s >> 8));
        buf.push_back(uint8_t(s));
    }
    return buf;
}

// Builds a frame with an arbitrary type byte and raw payload (for unknown-type tests)
static std::vector<uint8_t> make_frame(uint8_t type, std::vector<uint8_t> payload)
{
    std::vector<uint8_t> buf;
    buf.push_back(0xAD);
    buf.push_back(type);
    buf.push_back(uint8_t(payload.size() >> 8));
    buf.push_back(uint8_t(payload.size()));
    buf.insert(buf.end(), payload.begin(), payload.end());
    return buf;
}

TEST_CASE("parses a single valid STREAM_DATA frame", "[frame_parser]")
{
    FrameParser p;
    auto        frames = p.feed(make_stream_frame(0, 0, {100, 200, 300}));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].channel_id == 0);
    REQUIRE(frames[0].seq == 0);
    REQUIRE(frames[0].samples == std::vector<uint16_t>{100, 200, 300});
}

TEST_CASE("channel_id is captured correctly", "[frame_parser]")
{
    FrameParser p;
    auto        frames = p.feed(make_stream_frame(7, 0, {1}));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].channel_id == 7);
}

TEST_CASE("parses multiple back-to-back frames", "[frame_parser]")
{
    FrameParser p;
    auto        buf = make_stream_frame(0, 0, {1, 2});
    auto        f1  = make_stream_frame(0, 1, {3, 4});
    buf.insert(buf.end(), f1.begin(), f1.end());
    auto frames = p.feed(buf);
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].seq == 0);
    REQUIRE(frames[1].seq == 1);
}

TEST_CASE("buffers partial frame across feeds", "[frame_parser]")
{
    FrameParser p;
    auto        bytes   = make_stream_frame(0, 0, {42});
    auto        frames1 = p.feed(std::span(bytes.data(), 4));
    REQUIRE(frames1.empty());
    auto frames2 = p.feed(std::span(bytes.data() + 4, bytes.size() - 4));
    REQUIRE(frames2.size() == 1);
    REQUIRE(frames2[0].samples[0] == 42);
}

TEST_CASE("resyncs after leading garbage bytes", "[frame_parser]")
{
    FrameParser          p;
    std::vector<uint8_t> buf   = {0x01, 0xFF, 0x00};
    auto                 frame = make_stream_frame(0, 0, {7, 8});
    buf.insert(buf.end(), frame.begin(), frame.end());
    auto frames = p.feed(buf);
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].samples == std::vector<uint16_t>{7, 8});
}

TEST_CASE("skips unknown frame type using LENGTH field", "[frame_parser]")
{
    FrameParser p;
    // Unknown type 0x00 (DEVICE_INFO) with some payload, followed by a STREAM_DATA
    auto unknown = make_frame(0x00, {0x00, 0x01, 0x0F, 0x01});
    auto stream  = make_stream_frame(0, 5, {99});
    unknown.insert(unknown.end(), stream.begin(), stream.end());
    auto frames = p.feed(unknown);
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].seq == 5);
    REQUIRE(frames[0].samples[0] == 99);
}

TEST_CASE("skips multiple unknown types before STREAM_DATA", "[frame_parser]")
{
    FrameParser p;
    auto        buf = make_frame(0x10, {0x80, 0x00});                               // ACK frame
    auto        f2  = make_frame(0x02, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06}); // PERIODIC_DATA
    auto        f3  = make_stream_frame(2, 0, {55});
    buf.insert(buf.end(), f2.begin(), f2.end());
    buf.insert(buf.end(), f3.begin(), f3.end());
    auto frames = p.feed(buf);
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].channel_id == 2);
    REQUIRE(frames[0].samples[0] == 55);
}

TEST_CASE("detects seq drop", "[frame_parser]")
{
    FrameParser p;
    auto        buf = make_stream_frame(0, 0, {1});
    auto        f3  = make_stream_frame(0, 3, {2}); // seq 1 and 2 missing
    buf.insert(buf.end(), f3.begin(), f3.end());
    p.feed(buf);
    REQUIRE(p.seq_drops() == 2);
}

TEST_CASE("no drop for sequential frames", "[frame_parser]")
{
    FrameParser p;
    for (uint16_t i = 0; i < 8; ++i)
        p.feed(make_stream_frame(0, i, {i}));
    REQUIRE(p.seq_drops() == 0);
}

TEST_CASE("seq wraps from 0xFFFF to 0x0000 without false drop", "[frame_parser]")
{
    FrameParser p;
    auto        buf = make_stream_frame(0, 0xFFFE, {1});
    auto        f1  = make_stream_frame(0, 0xFFFF, {2});
    auto        f2  = make_stream_frame(0, 0x0000, {3});
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());
    p.feed(buf);
    REQUIRE(p.seq_drops() == 0);
}
