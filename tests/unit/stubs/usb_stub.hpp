#ifndef KVASIR_TESTS_UNIT_STUBS_USB_STUB_HPP
#define KVASIR_TESTS_UNIT_STUBS_USB_STUB_HPP

#include <cstdint>
#include <vector>

namespace kvasir {

const std::vector<std::vector<uint8_t>>& usb_stub_frames();
std::vector<std::vector<uint8_t>>        usb_stub_frames_of_type(uint8_t type);
void                                     usb_stub_reset();

} // namespace kvasir

#endif // KVASIR_TESTS_UNIT_STUBS_USB_STUB_HPP
