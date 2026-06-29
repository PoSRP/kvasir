#pragma once
#include "protocol.hpp"
#include <array>
#include <cstdint>
#include <string_view>

// Board-specific compile-time channel table.
// A different board variant replaces this file; firmware logic does not change.

struct ChannelDef {
    uint8_t          id;
    ChannelType      type;
    std::string_view name;
};

inline constexpr std::array<ChannelDef, 15> CHANNELS = {{
    {0, ChannelType::STREAM, "ADC_IN0"},
    {1, ChannelType::STREAM, "ADC_IN1"},
    {2, ChannelType::STREAM, "ADC_IN2"},
    {3, ChannelType::STREAM, "ADC_IN3"},
    {4, ChannelType::STREAM, "ADC_IN5"}, // IN4 used for SPI1_NSS (PA4)
    {5, ChannelType::STREAM, "ADC_IN6"},
    {6, ChannelType::STREAM, "ADC_IN7"},
    {7, ChannelType::STREAM, "ADC_IN8"},
    {8, ChannelType::STREAM, "ADC_IN9"},
    {9, ChannelType::PERIODIC, "SPI1"},
    {10, ChannelType::PERIODIC, "SPI2"},
    {11, ChannelType::PERIODIC, "I2C1"},
    {12, ChannelType::PERIODIC, "I2C2"},
    {13, ChannelType::PERIODIC, "I2C3"},
    {14, ChannelType::PERIODIC, "USART1"},
}};
