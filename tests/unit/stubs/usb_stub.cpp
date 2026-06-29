#include "interfaces/iusb.hpp"
#include "usb_stub.hpp"

static std::vector<std::vector<uint8_t>> g_frames;

class UsbStub final : public IUsbHal {
public:
    bool transmit(std::span<const uint8_t> data) override
    {
        g_frames.emplace_back(data.begin(), data.end());
        return true;
    }
    bool tx_ready() const override { return true; }
};

IUsbHal& usb_hal()
{
    static UsbStub s_stub;
    return s_stub;
}

const std::vector<std::vector<uint8_t>>& usb_stub_frames() { return g_frames; }

std::vector<std::vector<uint8_t>> usb_stub_frames_of_type(uint8_t type)
{
    std::vector<std::vector<uint8_t>> out;
    for (const auto& f : g_frames)
        if (f.size() >= 2 && f[1] == type)
            out.push_back(f);
    return out;
}

void usb_stub_reset() { g_frames.clear(); }
