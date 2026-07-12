#include <catch2/catch_test_macros.hpp>
#include "app.hpp"
#include "frame.hpp"
#include "protocol.hpp"
#include "stubs/usb_stub.hpp"
#include <vector>
#include <cstdint>

using namespace kvasir;

static uint16_t read_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8u) | p[1]);
}

static std::vector<uint8_t> make_cmd(FrameType type, std::vector<uint8_t> payload = {})
{
    std::vector<uint8_t> f;
    f.push_back(0xAD);
    f.push_back(static_cast<uint8_t>(type));
    f.push_back(static_cast<uint8_t>(payload.size() >> 8u));
    f.push_back(static_cast<uint8_t>(payload.size()));
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

static std::vector<uint8_t> make_config_ch0(bool enabled = true, uint8_t samp_time = 0)
{
    std::vector<uint8_t> ch = {0, 2, static_cast<uint8_t>(enabled), samp_time};
    return make_cmd(FrameType::CONFIG, ch);
}

static void inject(const std::vector<uint8_t>& frame)
{
    app_usb_rx(frame.data(), static_cast<uint32_t>(frame.size()));
}

static void full_setup()
{
    app_reset();
    app_init();
    usb_stub_reset();

    app_on_connect();
    app_process();

    inject(make_config_ch0());
    app_process();
    usb_stub_reset();

    inject(make_cmd(FrameType::START));
    app_process();
    usb_stub_reset();
}

TEST_CASE("connect sends nothing until host commands arrive", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    REQUIRE(usb_stub_frames().empty());
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

TEST_CASE("CONFIG in UNCONFIGURED state sends OK ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    inject(make_config_ch0());
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == static_cast<uint8_t>(CmdType::CONFIG));
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::OK));
}

TEST_CASE("CONFIG while RUNNING sends ERROR", "[pipeline]")
{
    full_setup();
    inject(make_config_ch0());
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::ERROR));
}

TEST_CASE("START before CONFIG sends ERROR", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    inject(make_cmd(FrameType::START));
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == static_cast<uint8_t>(CmdType::START));
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::ERROR));
}

TEST_CASE("START after CONFIG sends OK ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    inject(make_config_ch0());
    app_process();
    usb_stub_reset();
    inject(make_cmd(FrameType::START));
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == static_cast<uint8_t>(CmdType::START));
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::OK));
}

TEST_CASE("STOP while RUNNING sends OK ACK", "[pipeline]")
{
    full_setup();
    inject(make_cmd(FrameType::STOP));
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][4] == static_cast<uint8_t>(CmdType::STOP));
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::OK));
}

TEST_CASE("no STREAM_DATA before SAMPLES_PER_FRAME accumulated", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME - 1; i++) {
        app_adc_push(static_cast<uint16_t>(i));
    }
    app_process();
    REQUIRE(usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA)).empty());
}

TEST_CASE("STREAM_DATA sent after SAMPLES_PER_FRAME samples", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        app_adc_push(static_cast<uint16_t>(i));
    }
    app_process();
    auto frames = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA));
    REQUIRE(frames.size() == 1);
    const auto& f = frames[0];
    REQUIRE(f[0] == 0xAD);
    REQUIRE(f[1] == static_cast<uint8_t>(FrameType::STREAM_DATA));
    REQUIRE(f[4] == 0);
}

TEST_CASE("STREAM_DATA sample count field is correct", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        app_adc_push(static_cast<uint16_t>(i));
    }
    app_process();
    const auto f = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA))[0];
    REQUIRE(read_be16(&f[7]) == SAMPLES_PER_FRAME);
}

TEST_CASE("STREAM_DATA seq increments across frames", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME * 2; i++) {
        app_adc_push(static_cast<uint16_t>(i));
    }
    app_process();
    app_process();
    auto frames = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA));
    REQUIRE(frames.size() == 2);
    uint16_t s0 = read_be16(&frames[0][5]);
    uint16_t s1 = read_be16(&frames[1][5]);
    REQUIRE(s1 == s0 + 1);
}

TEST_CASE("STREAM_DATA sample values are preserved big-endian", "[pipeline]")
{
    full_setup();
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        app_adc_push(static_cast<uint16_t>(i * 10));
    }
    app_process();
    const auto f = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA))[0];
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        uint16_t got = read_be16(&f[9 + (i * 2)]);
        REQUIRE(got == static_cast<uint16_t>(i * 10));
    }
}

TEST_CASE("no STREAM_DATA when not RUNNING", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    inject(make_config_ch0());
    app_process();
    usb_stub_reset();

    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        app_adc_push(static_cast<uint16_t>(i));
    }
    app_process();
    REQUIRE(usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::STREAM_DATA)).empty());
}

TEST_CASE("unknown command receives ERROR ACK", "[pipeline]")
{
    app_reset();
    app_init();
    usb_stub_reset();
    app_on_connect();
    app_process();
    usb_stub_reset();
    inject(make_cmd(static_cast<FrameType>(0x7F)));
    app_process();
    auto acks = usb_stub_frames_of_type(static_cast<uint8_t>(FrameType::ACK));
    REQUIRE(!acks.empty());
    REQUIRE(acks[0][5] == static_cast<uint8_t>(AckStatus::ERROR));
}
