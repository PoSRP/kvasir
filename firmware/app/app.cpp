#include "app.hpp"
#include "frame.hpp"
#include "protocol.hpp"
#include "ring_buffer.hpp"
#include "interfaces/iadc.hpp"
#include "interfaces/iusb.hpp"
#include "ymir/api.h"
#include <array>
#include <cstring>
#include <optional>
#include <span>

// Provided by firmware/app/hal/ or tests/unit/stubs/ at link time
IAdcHal&        adc_hal();
IUsbHal&        usb_hal();
extern "C" void led_tick_1hz();

/* === State === */

enum class AppState : uint8_t { UNCONFIGURED, CONFIGURED, RUNNING };

static constexpr uint8_t NUM_STREAM_CH = 9;

static std::array<RingBuffer<uint16_t, 2048>, NUM_STREAM_CH>              g_adc_ring;
static RingBuffer<uint8_t, 512>                                           g_rx_ring;
static std::array<FramePacker, NUM_STREAM_CH>                             g_packer;
static IUsbHal*                                                           g_usb = nullptr;
static IAdcHal*                                                           g_adc = nullptr;
static std::array<std::array<uint16_t, SAMPLES_PER_FRAME>, NUM_STREAM_CH> g_samples;
static std::array<size_t, NUM_STREAM_CH>                                  g_count{};
static AppState      g_state       = AppState::UNCONFIGURED;
static bool          g_prev_online = false;
static volatile bool g_host_online = false;

static std::array<bool, NUM_STREAM_CH>    g_ch_enabled{};
static std::array<uint8_t, NUM_STREAM_CH> g_ch_samp_time{};
static volatile bool                      g_send_info = false;

/* === ISR safe functions === */

extern "C" void app_adc_push_ch(uint8_t ch_id, uint16_t s)
{
    if (ch_id < NUM_STREAM_CH)
        g_adc_ring[ch_id].push(s);
}

extern "C" void app_adc_push(uint16_t s) { app_adc_push_ch(0, s); }

// Sliding 8-byte window over raw RX bytes, matching the ymir test-app pattern.
// On a CRC-validated DFU enter-update request the MCU resets into the bootloader.
static std::array<uint8_t, 8> g_dfu_window{};
static uint32_t               g_dfu_pos = 0;

extern "C" void app_usb_rx(const uint8_t* buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        g_rx_ring.push(buf[i]);

        g_dfu_window[g_dfu_pos++] = buf[i];
        if (g_dfu_pos == 8) {
            if (ymir::is_enter_update_request(std::span<const uint8_t, 8>(g_dfu_window))) {
                ymir::enter_update();
            }
            std::memmove(g_dfu_window.data(), g_dfu_window.data() + 1, 7);
            g_dfu_pos = 7;
        }
    }
}
extern "C" void app_on_connect() { g_host_online = true; }
extern "C" void app_on_disconnect() { g_host_online = false; }
extern "C" void app_on_usb_init() { g_send_info = true; }

/* === USB protocol parser === */

class CmdParser {
public:
    struct Cmd {
        uint8_t        type;
        const uint8_t* payload;
        uint16_t       len;
    };

    std::optional<Cmd> push(uint8_t b)
    {
        switch (st_) {
        case St::MAGIC:
            if (b == 0xAD)
                st_ = St::TYPE;
            break;
        case St::TYPE:
            type_ = b;
            st_   = St::LEN_HI;
            break;
        case St::LEN_HI:
            len_ = static_cast<uint16_t>(b) << 8;
            st_  = St::LEN_LO;
            break;
        case St::LEN_LO:
            len_ |= b;
            if (len_ == 0) {
                st_ = St::MAGIC;
                return Cmd{type_, buf_.data(), 0};
            }
            if (len_ > kMax) {
                st_ = St::MAGIC;
                break;
            }
            got_ = 0;
            st_  = St::PAYLOAD;
            break;
        case St::PAYLOAD:
            buf_[got_++] = b;
            if (got_ == len_) {
                st_ = St::MAGIC;
                return Cmd{type_, buf_.data(), len_};
            }
            break;
        }
        return std::nullopt;
    }

    void reset() { st_ = St::MAGIC; }

private:
    enum class St : uint8_t { MAGIC, TYPE, LEN_HI, LEN_LO, PAYLOAD };
    static constexpr uint16_t kMax  = 256;
    St                        st_   = St::MAGIC;
    uint8_t                   type_ = 0;
    uint16_t                  len_  = 0;
    uint16_t                  got_  = 0;
    std::array<uint8_t, kMax> buf_{};
};

static CmdParser g_cmd_parser;

/* === Helpers === */

static void do_transmit(std::span<uint8_t> buf, size_t n)
{
    if (n > 0)
        g_usb->transmit(buf.first(n));
}

/* === Command handlers === */

static void handle_config(const uint8_t* payload, uint16_t len)
{
    if (g_state == AppState::RUNNING) {
        std::array<uint8_t, 16> buf{};
        do_transmit(buf, pack_ack(CmdType::CONFIG, AckStatus::ERR_BAD_STATE, buf));
        return;
    }

    // Parse per-channel config records: [ch_id(1) cfg_len(1) cfg_data(N)]...
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t ch_id   = payload[i];
        uint8_t cfg_len = payload[i + 1];
        i += 2;
        if (i + cfg_len > len)
            break;

        if (ch_id < NUM_STREAM_CH && cfg_len >= 2) {
            // STREAM channel: enabled(1) sampling_time(1)
            g_ch_enabled[ch_id]   = payload[i];
            g_ch_samp_time[ch_id] = payload[i + 1];
        }
        i += cfg_len;
    }

    g_state = AppState::CONFIGURED;

    // Collect enabled channels and their sampling times to compute the scan rate.
    std::array<uint8_t, NUM_STREAM_CH> samp_times{};
    uint8_t                            enabled_count = 0;
    for (uint8_t id = 0; id < NUM_STREAM_CH; ++id) {
        if (g_ch_enabled[id])
            samp_times[enabled_count++] = g_ch_samp_time[id];
    }
    uint32_t ch_rate =
        g_adc->compute_rate(std::span<const uint8_t>(samp_times.data(), enabled_count));

    // Reply with actual achieved config for each enabled STREAM channel.
    std::array<uint8_t, NUM_STREAM_CH * 6> entries{};
    size_t                                 entries_len = 0;
    for (uint8_t id = 0; id < NUM_STREAM_CH; ++id) {
        if (!g_ch_enabled[id])
            continue;
        entries[entries_len++] = id;
        entries[entries_len++] = 4;
        entries[entries_len++] = static_cast<uint8_t>(ch_rate >> 24);
        entries[entries_len++] = static_cast<uint8_t>(ch_rate >> 16);
        entries[entries_len++] = static_cast<uint8_t>(ch_rate >> 8);
        entries[entries_len++] = static_cast<uint8_t>(ch_rate);
    }
    std::array<uint8_t, 64> buf{};
    do_transmit(buf, pack_ack_config(AckStatus::OK,
                                     std::span<const uint8_t>(entries.data(), entries_len), buf));
}

static void handle_start()
{
    std::array<uint8_t, 16> buf{};
    if (g_state != AppState::CONFIGURED) {
        do_transmit(buf, pack_ack(CmdType::START, AckStatus::ERR_BAD_STATE, buf));
        return;
    }

    // Collect enabled STREAM channel IDs and sampling times in order.
    std::array<uint8_t, NUM_STREAM_CH> enabled_ids{};
    std::array<uint8_t, NUM_STREAM_CH> samp_times{};
    uint8_t                            count = 0;
    for (uint8_t id = 0; id < NUM_STREAM_CH; ++id) {
        if (g_ch_enabled[id]) {
            enabled_ids[count] = id;
            samp_times[count]  = g_ch_samp_time[id];
            count++;
        }
    }

    g_adc->start(std::span<const uint8_t>(enabled_ids.data(), count),
                 std::span<const uint8_t>(samp_times.data(), count));
    g_state = AppState::RUNNING;
    do_transmit(buf, pack_ack(CmdType::START, AckStatus::OK, buf));
}

static void handle_stop()
{
    if (g_state == AppState::RUNNING)
        g_adc->stop();
    g_state = AppState::UNCONFIGURED;
    g_ch_enabled.fill(false);
    for (auto& p : g_packer)
        p.reset();
    g_count.fill(0);
    for (auto& ring : g_adc_ring)
        while (ring.pop()) {
        }
    std::array<uint8_t, 16> buf{};
    do_transmit(buf, pack_ack(CmdType::STOP, AckStatus::OK, buf));
    g_send_info = true;
}

static void dispatch(uint8_t type, const uint8_t* payload, uint16_t len)
{
    switch (static_cast<FrameType>(type)) {
    case FrameType::CONFIG:
        handle_config(payload, len);
        break;
    case FrameType::START:
        handle_start();
        break;
    case FrameType::STOP:
        handle_stop();
        break;
    case FrameType::DFU: {
        std::array<uint8_t, 16> buf{};
        do_transmit(buf, pack_ack(CmdType::DFU, AckStatus::OK, buf));
        break;
    }
    default: {
        std::array<uint8_t, 16> buf{};
        do_transmit(buf, pack_ack(static_cast<CmdType>(type), AckStatus::ERR_INVALID, buf));
        break;
    }
    }
}

/* === The actual application logic === */

void app_init()
{
    ymir::confirm_boot();
    g_usb = &usb_hal();
    g_adc = &adc_hal();
}

void app_reset()
{
    for (auto& ring : g_adc_ring)
        while (ring.pop()) {
        }
    while (g_rx_ring.pop()) {
    }
    for (auto& p : g_packer)
        p.reset();
    g_count.fill(0);
    g_ch_enabled.fill(false);
    g_cmd_parser.reset();
    g_send_info   = false;
    g_state       = AppState::UNCONFIGURED;
    g_prev_online = false;
}

void app_process()
{
    ymir::feed_watchdog();
    led_tick_1hz();

    bool online = g_host_online;

    if (online && !g_prev_online) {
        // New connection: reset state, send DEVICE_INFO.
        if (g_state == AppState::RUNNING)
            g_adc->stop();
        g_state = AppState::UNCONFIGURED;
        for (auto& p : g_packer)
            p.reset();
        g_count.fill(0);
        for (auto& ring : g_adc_ring)
            while (ring.pop()) {
            }
        std::array<uint8_t, 256> buf{};
        do_transmit(buf, pack_device_info(buf));
    } else if (!online && g_prev_online) {
        // Disconnection: stop capture, reset state.
        if (g_state == AppState::RUNNING)
            g_adc->stop();
        g_state = AppState::UNCONFIGURED;
        for (auto& p : g_packer)
            p.reset();
        g_count.fill(0);
        for (auto& ring : g_adc_ring)
            while (ring.pop()) {
            }
        g_cmd_parser.reset();
    }
    g_prev_online = online;

    // Drain RX ring and dispatch commands.
    while (auto b = g_rx_ring.pop()) {
        if (auto cmd = g_cmd_parser.push(*b))
            dispatch(cmd->type, cmd->payload, cmd->len);
    }

    // Send DEVICE_INFO when requested (set by USB init, STOP, or DTR assert).
    if (g_send_info && g_usb->tx_ready()) {
        g_send_info = false;
        std::array<uint8_t, 256> buf{};
        do_transmit(buf, pack_device_info(buf));
    }

    // Emit STREAM_DATA frames when SAMPLES_PER_FRAME samples have accumulated.
    if (g_state == AppState::RUNNING) {
        for (uint8_t ch_id = 0; ch_id < NUM_STREAM_CH; ++ch_id) {
            if (!g_ch_enabled[ch_id])
                continue;
            // Accumulate only if frame not yet full (holds samples when USB is busy).
            if (g_count[ch_id] < SAMPLES_PER_FRAME) {
                while (auto s = g_adc_ring[ch_id].pop()) {
                    g_samples[ch_id][g_count[ch_id]++] = *s;
                    if (g_count[ch_id] == SAMPLES_PER_FRAME)
                        break;
                }
            }
            // Only pack (seq++) when USB is confirmed free; no seq gaps on busy USB.
            if (g_count[ch_id] == SAMPLES_PER_FRAME && g_usb->tx_ready()) {
                std::array<uint8_t, FRAME_HEADER_SIZE + 1 + 2 + 2 + SAMPLES_PER_FRAME * 2> buf{};
                do_transmit(buf, g_packer[ch_id].pack(ch_id, g_samples[ch_id], buf));
                g_count[ch_id] = 0;
            }
        }
    }
}

void app_main()
{
    app_init();
    for (;;)
        app_process();
}
