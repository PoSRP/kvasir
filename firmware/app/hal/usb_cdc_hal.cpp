#include "interfaces/iusb.hpp"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "stm32f4xx_hal.h"
#include <algorithm>
#include <array>

extern "C" USBD_HandleTypeDef hUsbDeviceFS;

static std::array<uint8_t, APP_TX_DATA_SIZE> tx_buf;

class UsbHal final : public IUsbHal {
public:
    bool tx_ready() const override
    {
        const auto* hcdc = static_cast<const USBD_CDC_HandleTypeDef*>(hUsbDeviceFS.pClassData);
        return hcdc != nullptr && hcdc->TxState == 0;
    }

    bool transmit(std::span<const uint8_t> data) override
    {
        auto* hcdc = static_cast<USBD_CDC_HandleTypeDef*>(hUsbDeviceFS.pClassData);
        if (!hcdc || hcdc->TxState != 0)
            return false;
        std::copy(data.begin(), data.end(), tx_buf.begin());
        return CDC_Transmit_FS(tx_buf.data(), static_cast<uint16_t>(data.size())) == USBD_OK;
    }
};

IUsbHal& usb_hal()
{
    static UsbHal s_hal;
    return s_hal;
}
