#include <catch2/catch_test_macros.hpp>
#include "app.hpp"
#include "frame.hpp"
#include "protocol.hpp"
#include "stubs/usb_stub.hpp"
#include <vector>
#include <cstdint>

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint16_t read_be16(const uint8_t* p) { return uint16_t(p[0] << 8) | p[1]; }

static std::vector<uint8_t> make_cmd(FrameType type, std::vector<uint8_t> payload = {})
{
    std::vector<uint8_t> f;
    f.push_back(0xAD);
    f.push_back(uint8_t(type));
    f.push_back(uint8_t(payload.size() >> 8));
    f.push_back(uint8_t(payload.size()));
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

static std::vector<uint8_t> make_config_ch0(bool enabled = true, uint8_t samp_time = 0)
{
    // CONFIG payload: [ch_id(1) cfg_len(1) enabled(1) sampling_time(1)]
    std::vector<uint8_t> ch = {0, 2, uint8_t(enabled), samp_time};
    return make_cmd(FrameType::CONFIG, ch);
}

static void inject(const std::vector<uint8_t>& frame)
{
    app_usb_rx(frame.data(), uint32_t(frame.size()));
}

// Full session setup: connect, configure, start.
// Clears the stub between each phase so only data frames remain after.
static void full_setup()
{
    app_reset();
    app_init();
    usb_stub_reset();

    app_on_connect();
    app_process();
    usb_stub_reset(); // clear DEVICE_INFO

    inject(make_config_ch0());
    app_process();
    usb_stub_reset(); // clear CONFIG ACK

    inject(make_cmd(FrameType::START));
    app_process();
    usb_stub_reset(); // clear START ACK
}

// ── Connection / DEVICE_INFO ──────────────────────────────────────────────────

TEST_CASE("connect sends DEVICE_INFO", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    auto frames = usb_stub_frames_of_type(uint8_t(FrameType::DEVICE_INFO));
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0][0] == 0xAD);
    REQUIRE(frames[0][6] == 15); // 15 channels
}

TEST_CASE("disconnect without connect does not crash", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_disconnect();
    app_process();
    REQUIRE(usb_stub_frames().empty());
}

// ── CONFIG command ────────────────────────────────────────────────────────────

TEST_CASE("CONFIG in UNCONFIGURED state sends OK ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_config_ch0());
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == uint8_t(CmdType::CONFIG));
    REQUIRE(acks[0][5] == uint8_t(AckStatus::OK));
}

TEST_CASE("CONFIG while RUNNING sends ERR_BAD_STATE", "[pipeline]")
{
    full_setup();
    inject(make_config_ch0());
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][5] == uint8_t(AckStatus::ERR_BAD_STATE));
}

// ── START / STOP ──────────────────────────────────────────────────────────────

TEST_CASE("START before CONFIG sends ERR_BAD_STATE", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_cmd(FrameType::START));
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == uint8_t(CmdType::START));
    REQUIRE(acks[0][5] == uint8_t(AckStatus::ERR_BAD_STATE));
}

TEST_CASE("START after CONFIG sends OK ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_config_ch0());
    app_process();
    usb_stub_reset();
    inject(make_cmd(FrameType::START));
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == uint8_t(CmdType::START));
    REQUIRE(acks[0][5] == uint8_t(AckStatus::OK));
}

TEST_CASE("STOP while RUNNING sends OK ACK", "[pipeline]")
{
    full_setup();
    inject(make_cmd(FrameType::STOP));
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == uint8_t(CmdType::STOP));
    REQUIRE(acks[0][5] == uint8_t(AckStatus::OK));
}

// ── STREAM_DATA ───────────────────────────────────────────────────────────────

TEST_CASE("no STREAM_DATA before SAMPLES_PER_FRAME accumulated", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME - 1; i++)
        app_adc_push(uint16_t(i));
    app_process();
    REQUIRE(usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA)).empty());
}

TEST_CASE("STREAM_DATA sent after SAMPLES_PER_FRAME samples", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++)
        app_adc_push(uint16_t(i));
    app_process();
    auto frames = usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA));
    REQUIRE(frames.size() == 1);
    const auto& f = frames[0];
    REQUIRE(f[0] == 0xAD);
    REQUIRE(f[1] == uint8_t(FrameType::STREAM_DATA));
    REQUIRE(f[4] == 0); // channel_id = 0
}

TEST_CASE("STREAM_DATA sample count field is correct", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++)
        app_adc_push(uint16_t(i));
    app_process();
    const auto f = usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA))[0];
    REQUIRE(read_be16(&f[7]) == SAMPLES_PER_FRAME);
}

TEST_CASE("STREAM_DATA seq increments across frames", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME * 2; i++)
        app_adc_push(uint16_t(i));
    // app_process emits at most one frame per channel per call.
    app_process();
    app_process();
    auto frames = usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA));
    REQUIRE(frames.size() == 2);
    uint16_t s0 = read_be16(&frames[0][5]);
    uint16_t s1 = read_be16(&frames[1][5]);
    REQUIRE(s1 == s0 + 1);
}

TEST_CASE("STREAM_DATA sample values are preserved big-endian", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++)
        app_adc_push(uint16_t(i * 10));
    app_process();
    const auto f = usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA))[0];
    // Samples start at byte 9 (header 4 + ch_id 1 + seq 2 + count 2)
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        uint16_t got = read_be16(&f[9 + i * 2]);
        REQUIRE(got == uint16_t(i * 10));
    }
}

TEST_CASE("no STREAM_DATA when not RUNNING", "[pipeline]")
{
    // Only configure, don't start
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_config_ch0());
    app_process();
    usb_stub_reset();

    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++)
        app_adc_push(uint16_t(i));
    app_process();
    REQUIRE(usb_stub_frames_of_type(uint8_t(FrameType::STREAM_DATA)).empty());
}

TEST_CASE("unknown command receives ERR_INVALID ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_cmd(FrameType(0x7F)));
    app_process();
    auto acks = usb_stub_frames_of_type(uint8_t(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][5] == uint8_t(AckStatus::ERR_INVALID));
}
