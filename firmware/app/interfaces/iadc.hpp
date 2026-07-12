#ifndef KVASIR_INTERFACES_IADC_HPP
#define KVASIR_INTERFACES_IADC_HPP

#include <cstdint>
#include <span>

namespace kvasir {

class IAdcHal {
public:
    IAdcHal()                          = default;
    IAdcHal(const IAdcHal&)            = delete;
    IAdcHal(IAdcHal&&)                 = delete;
    IAdcHal& operator=(const IAdcHal&) = delete;
    IAdcHal& operator=(IAdcHal&&)      = delete;
    virtual ~IAdcHal()                 = default;

    virtual void start(std::span<const uint8_t> ch_ids,
                       std::span<const uint8_t> sampling_times) = 0;
    virtual void stop()                                         = 0;
};

IAdcHal& adc_hal();

} // namespace kvasir

#endif // KVASIR_INTERFACES_IADC_HPP
