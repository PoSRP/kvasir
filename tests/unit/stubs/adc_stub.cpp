#include "interfaces/iadc.hpp"

namespace kvasir {

class AdcStub final : public IAdcHal {
public:
    void start(std::span<const uint8_t>, std::span<const uint8_t>) override {}
    void stop() override {}
};

IAdcHal& adc_hal()
{
    static AdcStub s_stub;
    return s_stub;
}

} // namespace kvasir
