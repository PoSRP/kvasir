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

namespace kvasir {

extern "C" void led_tick_1hz();

/* === State === */

enum class AppState : uint8_t { UNCONFIGURED, CONFIGURED, RUNNING };

static constexpr uint8_t NUM_CHANNELS = 10;

static std::array<RingBuffer<uint16_t, 2048>, NUM_CHANNELS>              g_adc_ring;
static RingBuffer<uint8_t, 512>                                          g_rx_ring;
static std::array<FramePacker, NUM_CHANNELS>                             g_packer;
static IUsbHal*                                                          g_usb = nullptr;
static IAdcHal*                                                          g_adc = nullptr;
static std::array<std::array<uint16_t, SAMPLES_PER_FRAME>, NUM_CHANNELS> g_samples;
static std::array<size_t, NUM_CHANNELS>                                  g_count{};
static AppState      g_state       = AppState::UNCONFIGURED;
static bool          g_prev_online = false;
static volatile bool g_host_online = false;

static std::array<bool, NUM_CHANNELS>    g_channel_enabled{};
static std::array<uint8_t, NUM_CHANNELS> g_channel_sampling_time{};

/* === ISR safe functions === */

extern "C" void app_adc_push_ch(uint8_t channel_id, uint16_t sample)
{
    if (channel_id < NUM_CHANNELS) {
        g_adc_ring[channel_id].push(sample);
    }
}

extern "C" void app_adc_push(uint16_t sample) { app_adc_push_ch(0, sample); }

// Sliding 8-byte window over raw RX bytes, matching the ymir test-app pattern.
// On a CRC-validated DFU enter-update request the MCU resets into the bootloader.
static std::array<uint8_t, 8> g_dfu_window{};
static uint32_t               g_dfu_pos = 0;

extern "C" void app_usb_rx(const uint8_t* buffer, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        g_rx_ring.push(buffer[i]);

        g_dfu_window[g_dfu_pos++] = buffer[i];
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
extern "C" void app_on_usb_init() {}

/* === USB protocol parser === */

class CommandParser {
public:
    struct Command {
        uint8_t        type;
        const uint8_t* payload;
        uint16_t       length;
    };

    std::optional<Command> push(uint8_t byte)
    {
        switch (_state) {
        case State::WAIT_FOR_MAGIC:
            if (byte == 0xAD) {
                _state = State::READ_TYPE;
            }
            break;
        case State::READ_TYPE:
            _type  = byte;
            _state = State::READ_LENGTH_HIGH_BYTE;
            break;
        case State::READ_LENGTH_HIGH_BYTE:
            _length = static_cast<uint16_t>(static_cast<uint32_t>(byte) << 8u);
            _state  = State::READ_LENGTH_LOW_BYTE;
            break;
        case State::READ_LENGTH_LOW_BYTE:
            _length |= byte;
            if (_length == 0) {
                _state = State::WAIT_FOR_MAGIC;
                return Command{.type = _type, .payload = _buffer.data(), .length = 0};
            }
            if (_length > MAX_PAYLOAD_SIZE) {
                _state = State::WAIT_FOR_MAGIC;
                break;
            }
            _bytes_received = 0;
            _state          = State::READ_PAYLOAD;
            break;
        case State::READ_PAYLOAD:
            _buffer[_bytes_received++] = byte;
            if (_bytes_received == _length) {
                _state = State::WAIT_FOR_MAGIC;
                return Command{.type = _type, .payload = _buffer.data(), .length = _length};
            }
            break;
        }
        return std::nullopt;
    }

    void reset() { _state = State::WAIT_FOR_MAGIC; }

private:
    enum class State : uint8_t {
        WAIT_FOR_MAGIC,
        READ_TYPE,
        READ_LENGTH_HIGH_BYTE,
        READ_LENGTH_LOW_BYTE,
        READ_PAYLOAD,
    };
    static constexpr uint16_t             MAX_PAYLOAD_SIZE = 256;
    State                                 _state           = State::WAIT_FOR_MAGIC;
    uint8_t                               _type            = 0;
    uint16_t                              _length          = 0;
    uint16_t                              _bytes_received  = 0;
    std::array<uint8_t, MAX_PAYLOAD_SIZE> _buffer{};
};

static CommandParser g_command_parser;

/* === Helpers === */

static void do_transmit(std::span<uint8_t> buffer, size_t length)
{
    if (length > 0) {
        g_usb->transmit(buffer.first(length));
    }
}

/* === Command handlers === */

static void handle_config(const uint8_t* payload, uint16_t length)
{
    if (g_state == AppState::RUNNING) {
        std::array<uint8_t, 16> buffer{};
        do_transmit(buffer, pack_ack(CmdType::CONFIG, AckStatus::ERROR, buffer));
        return;
    }

    size_t i = 0;
    while (i + 2 <= length) {
        uint8_t channel_id    = payload[i];
        uint8_t config_length = payload[i + 1];
        i += 2;
        if (i + config_length > length) {
            break;
        }

        if (channel_id < NUM_CHANNELS && config_length >= 2) {
            g_channel_enabled[channel_id]       = payload[i] != 0;
            g_channel_sampling_time[channel_id] = payload[i + 1];
        }
        i += config_length;
    }

    g_state = AppState::CONFIGURED;

    std::array<uint8_t, 16> buffer{};
    do_transmit(buffer, pack_ack(CmdType::CONFIG, AckStatus::OK, buffer));
}

static void handle_start()
{
    std::array<uint8_t, 16> buffer{};
    if (g_state != AppState::CONFIGURED) {
        do_transmit(buffer, pack_ack(CmdType::START, AckStatus::ERROR, buffer));
        return;
    }

    std::array<uint8_t, NUM_CHANNELS> enabled_channel_ids{};
    std::array<uint8_t, NUM_CHANNELS> sampling_times{};
    uint8_t                           enabled_count = 0;
    for (uint8_t channel_id = 0; channel_id < NUM_CHANNELS; ++channel_id) {
        if (g_channel_enabled[channel_id]) {
            enabled_channel_ids[enabled_count] = channel_id;
            sampling_times[enabled_count]      = g_channel_sampling_time[channel_id];
            enabled_count++;
        }
    }

    g_adc->start(std::span<const uint8_t>(enabled_channel_ids.data(), enabled_count),
                 std::span<const uint8_t>(sampling_times.data(), enabled_count));
    g_state = AppState::RUNNING;
    do_transmit(buffer, pack_ack(CmdType::START, AckStatus::OK, buffer));
}

static void handle_stop()
{
    if (g_state == AppState::RUNNING) {
        g_adc->stop();
    }
    g_state = AppState::UNCONFIGURED;
    g_channel_enabled.fill(false);
    for (auto& packer : g_packer) {
        packer.reset();
    }
    g_count.fill(0);
    for (auto& ring : g_adc_ring) {
        while (ring.pop()) {
        }
    }
    std::array<uint8_t, 16> buffer{};
    do_transmit(buffer, pack_ack(CmdType::STOP, AckStatus::OK, buffer));
}

static void dispatch(uint8_t frame_type, const uint8_t* payload, uint16_t length)
{
    switch (static_cast<FrameType>(frame_type)) {
    case FrameType::CONFIG:
        handle_config(payload, length);
        break;
    case FrameType::START:
        handle_start();
        break;
    case FrameType::STOP:
        handle_stop();
        break;
    default: {
        std::array<uint8_t, 16> buffer{};
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        do_transmit(buffer, pack_ack(static_cast<CmdType>(frame_type), AckStatus::ERROR, buffer));
        break;
    }
    }
}

/* === The actual application logic === */

extern "C" void app_init()
{
    ymir::confirm_boot();
    g_usb = &usb_hal();
    g_adc = &adc_hal();
}

extern "C" void app_reset()
{
    for (auto& ring : g_adc_ring) {
        while (ring.pop()) {
        }
    }
    while (g_rx_ring.pop()) {
    }
    for (auto& packer : g_packer) {
        packer.reset();
    }
    g_count.fill(0);
    g_channel_enabled.fill(false);
    g_command_parser.reset();
    g_state       = AppState::UNCONFIGURED;
    g_prev_online = false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
extern "C" void app_process()
{
    ymir::feed_watchdog();
    led_tick_1hz();

    bool online = g_host_online;

    if (online && !g_prev_online) {
        if (g_state == AppState::RUNNING) {
            g_adc->stop();
        }
        g_state = AppState::UNCONFIGURED;
        for (auto& packer : g_packer) {
            packer.reset();
        }
        g_count.fill(0);
        for (auto& ring : g_adc_ring) {
            while (ring.pop()) {
            }
        }
    } else if (!online && g_prev_online) {
        if (g_state == AppState::RUNNING) {
            g_adc->stop();
        }
        g_state = AppState::UNCONFIGURED;
        for (auto& packer : g_packer) {
            packer.reset();
        }
        g_count.fill(0);
        for (auto& ring : g_adc_ring) {
            while (ring.pop()) {
            }
        }
        g_command_parser.reset();
    }
    g_prev_online = online;

    while (auto byte = g_rx_ring.pop()) {
        if (auto command = g_command_parser.push(*byte)) {
            dispatch(command->type, command->payload, command->length);
        }
    }

    if (g_state == AppState::RUNNING) {
        for (uint8_t channel_id = 0; channel_id < NUM_CHANNELS; ++channel_id) {
            if (!g_channel_enabled[channel_id]) {
                continue;
            }
            if (g_count[channel_id] < SAMPLES_PER_FRAME) {
                while (auto sample = g_adc_ring[channel_id].pop()) {
                    g_samples[channel_id][g_count[channel_id]++] = *sample;
                    if (g_count[channel_id] == SAMPLES_PER_FRAME) {
                        break;
                    }
                }
            }
            if (g_count[channel_id] == SAMPLES_PER_FRAME && g_usb->tx_ready()) {
                std::array<uint8_t, FRAME_HEADER_SIZE + 1 + 2 + 2 + (SAMPLES_PER_FRAME * 2)>
                    buffer{};
                do_transmit(buffer,
                            g_packer[channel_id].pack(channel_id, g_samples[channel_id], buffer));
                g_count[channel_id] = 0;
            }
        }
    }
}

extern "C" void app_main()
{
    app_init();
    for (;;) {
        app_process();
    }
}

} // namespace kvasir
