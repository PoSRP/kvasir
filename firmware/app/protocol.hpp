#pragma once
#include <cstdint>

inline constexpr uint8_t FW_VERSION_MAJOR = 0;
inline constexpr uint8_t FW_VERSION_MINOR = 1;

enum class FrameType : uint8_t {
    // Device → host
    DEVICE_INFO   = 0x00,
    STREAM_DATA   = 0x01,
    PERIODIC_DATA = 0x02,
    EVENT_DATA    = 0x03,
    ACK           = 0x10,
    // Host → device
    CONFIG = 0x80,
    START  = 0x81,
    STOP   = 0x82,
    DFU    = 0xF0,
};

enum class ChannelType : uint8_t {
    STREAM   = 0x00,
    PERIODIC = 0x01,
    EVENT    = 0x02,
};

enum class CmdType : uint8_t {
    CONFIG = 0x80,
    START  = 0x81,
    STOP   = 0x82,
    DFU    = 0xF0,
};

enum class AckStatus : uint8_t {
    OK            = 0x00,
    ERR_INVALID   = 0x01,
    ERR_BAD_STATE = 0x02,
};

enum class SensorType : uint8_t {
    NONE = 0x00,
    // Additional sensor drivers added here as implemented
};

enum class ValueType : uint8_t {
    FLOAT_BE = 0x00, // IEEE 754 float, big-endian
};
