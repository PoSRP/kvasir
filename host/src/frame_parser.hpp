#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

class FrameParser {
public:
    struct Frame {
        uint8_t               channel_id;
        uint16_t              seq;
        std::vector<uint16_t> samples;
    };

    std::vector<Frame> feed(std::span<const uint8_t> data);
    uint32_t           seq_drops() const { return seq_drops_; }

private:
    static constexpr uint8_t MAGIC       = 0xAD;
    static constexpr uint8_t STREAM_DATA = 0x01;

    std::vector<uint8_t>    buf_;
    std::optional<uint16_t> last_seq_;
    uint32_t                seq_drops_ = 0;

    std::optional<Frame> try_parse();
};
