#pragma once
#include <cstdint>
#include <span>

class IUsbHal {
public:
    virtual bool transmit(std::span<const uint8_t> data) = 0;
    virtual bool tx_ready() const                        = 0;
    virtual ~IUsbHal()                                   = default;
};

IUsbHal& usb_hal();
