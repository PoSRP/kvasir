#include "interfaces/iadc.hpp"

class AdcStub final : public IAdcHal {
public:
    void     start(std::span<const uint8_t>, std::span<const uint8_t>) override {}
    void     stop() override {}
    uint32_t compute_rate(std::span<const uint8_t>) const override { return 0; }
};

IAdcHal& adc_hal()
{
    static AdcStub s_stub;
    return s_stub;
}
