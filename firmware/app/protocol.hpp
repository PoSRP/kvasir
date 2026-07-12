#ifndef KVASIR_PROTOCOL_HPP
#define KVASIR_PROTOCOL_HPP

#include <cstdint>

namespace kvasir {

enum class FrameType : uint8_t {
    // Device -> host
    STREAM_DATA = 0x01,
    ACK         = 0x10,
    // Host -> device
    CONFIG = 0x80,
    START  = 0x81,
    STOP   = 0x82,
};

enum class CmdType : uint8_t {
    CONFIG = 0x80,
    START  = 0x81,
    STOP   = 0x82,
};

enum class AckStatus : uint8_t {
    OK    = 0x00,
    ERROR = 0x01,
};

} // namespace kvasir

#endif // KVASIR_PROTOCOL_HPP
