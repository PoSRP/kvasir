#pragma once
#include <cstdint>
#include <vector>

const std::vector<std::vector<uint8_t>>& usb_stub_frames();
std::vector<std::vector<uint8_t>>        usb_stub_frames_of_type(uint8_t type);
void                                     usb_stub_reset();
