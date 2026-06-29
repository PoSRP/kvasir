#include "frame_parser.hpp"

std::optional<FrameParser::Frame> FrameParser::try_parse()
{
    while (true) {
        // Resync: drop bytes until buf_[0] == MAGIC or buffer empty
        while (!buf_.empty() && buf_[0] != MAGIC)
            buf_.erase(buf_.begin());

        // Need at least header: magic(1) + type(1) + length(2)
        if (buf_.size() < 4)
            return std::nullopt;

        uint8_t  type        = buf_[1];
        uint16_t payload_len = (uint16_t(buf_[2]) << 8) | buf_[3];
        size_t   frame_size  = 4 + payload_len;

        if (buf_.size() < frame_size)
            return std::nullopt;

        if (type != STREAM_DATA || payload_len < 5) {
            // Unknown type or malformed STREAM_DATA: skip via LENGTH, loop for next
            buf_.erase(buf_.begin(), buf_.begin() + frame_size);
            continue;
        }

        // STREAM_DATA payload: ch_id(1) seq(2BE) count(2BE) samples[](uint16BE)
        uint8_t  ch_id = buf_[4];
        uint16_t seq   = (uint16_t(buf_[5]) << 8) | buf_[6];
        uint16_t count = (uint16_t(buf_[7]) << 8) | buf_[8];

        if (payload_len != uint16_t(5 + count * 2u)) {
            buf_.erase(buf_.begin(), buf_.begin() + frame_size);
            continue;
        }

        Frame f;
        f.channel_id = ch_id;
        f.seq        = seq;
        f.samples.resize(count);
        for (size_t i = 0; i < count; ++i)
            f.samples[i] = (uint16_t(buf_[9 + i * 2]) << 8) | buf_[9 + i * 2 + 1];

        if (last_seq_) {
            uint16_t expected = uint16_t(*last_seq_ + 1);
            if (seq != expected)
                seq_drops_ += uint16_t(seq - expected);
        }
        last_seq_ = seq;

        buf_.erase(buf_.begin(), buf_.begin() + frame_size);
        return f;
    }
}

std::vector<FrameParser::Frame> FrameParser::feed(std::span<const uint8_t> data)
{
    buf_.insert(buf_.end(), data.begin(), data.end());

    std::vector<Frame> frames;
    while (auto f = try_parse())
        frames.push_back(std::move(*f));
    return frames;
}
