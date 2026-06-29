#pragma once
#include <cstdint>
#include <span>

class IAdcHal {
public:
    virtual void     start(std::span<const uint8_t> ch_ids,
                           std::span<const uint8_t> sampling_times)              = 0;
    virtual void     stop()                                                      = 0;
    virtual uint32_t compute_rate(std::span<const uint8_t> sampling_times) const = 0;
    virtual ~IAdcHal()                                                           = default;
};

IAdcHal& adc_hal();
