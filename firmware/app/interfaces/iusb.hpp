#ifndef KVASIR_INTERFACES_IUSB_HPP
#define KVASIR_INTERFACES_IUSB_HPP

#include <cstdint>
#include <span>

namespace kvasir {

class IUsbHal {
public:
    IUsbHal()                          = default;
    IUsbHal(const IUsbHal&)            = delete;
    IUsbHal(IUsbHal&&)                 = delete;
    IUsbHal& operator=(const IUsbHal&) = delete;
    IUsbHal& operator=(IUsbHal&&)      = delete;
    virtual ~IUsbHal()                 = default;

    virtual bool               transmit(std::span<const uint8_t> data) = 0;
    [[nodiscard]] virtual bool tx_ready() const                        = 0;
};

IUsbHal& usb_hal();

} // namespace kvasir

#endif // KVASIR_INTERFACES_IUSB_HPP
