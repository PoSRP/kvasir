
#include "interfaces/iadc.hpp"

#include "app.hpp"
#include "adc.h"
#include "mmio.hpp"

#include "stm32f4xx_hal.h"

#include <array>
#include <cstdint>

namespace kvasir {

static constexpr size_t DMA_BUFFER_LENGTH = 512;

static std::array<uint16_t, DMA_BUFFER_LENGTH> dma_buffer;
static DMA_HandleTypeDef                       hdma_adc1;

static constexpr std::array<uint32_t, 10> ADC_HARDWARE_CHANNEL_MAP = {
    ADC_CHANNEL_0, // ID 0 -> PA0
    ADC_CHANNEL_1, // ID 1 -> PA1
    ADC_CHANNEL_2, // ID 2 -> PA2
    ADC_CHANNEL_3, // ID 3 -> PA3
    ADC_CHANNEL_4, // ID 4 -> PA4
    ADC_CHANNEL_5, // ID 5 -> PA5
    ADC_CHANNEL_6, // ID 6 -> PA6
    ADC_CHANNEL_7, // ID 7 -> PA7
    ADC_CHANNEL_8, // ID 8 -> PB0
    ADC_CHANNEL_9, // ID 9 -> PB1
};

static std::array<uint8_t, 10> s_scan_channel_ids;
static uint8_t                 s_scan_channel_count = 1;

static void push_range(size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i) {
        app_adc_push_ch(s_scan_channel_ids[i % s_scan_channel_count], dma_buffer[i]);
    }
}

class AdcHal final : public IAdcHal {
public:
    void start(std::span<const uint8_t> channel_ids,
               std::span<const uint8_t> sampling_times) override
    {
        if (channel_ids.empty()) {
            return;
        }

        s_scan_channel_count = static_cast<uint8_t>(channel_ids.size());
        for (size_t i = 0; i < channel_ids.size(); ++i) {
            s_scan_channel_ids[i] = channel_ids[i];
        }

        enable_dma2_clock();

        hdma_adc1.Instance                 = periph_ptr<DMA_Stream_TypeDef>(DMA2_Stream0_BASE);
        hdma_adc1.Init.Channel             = DMA_CHANNEL_0;
        hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
        hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
        hdma_adc1.Init.Mode                = DMA_CIRCULAR;
        hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
        hdma_adc1.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
        HAL_DMA_Init(&hdma_adc1);

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while)
        __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

        HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

        auto channel_count               = static_cast<uint8_t>(channel_ids.size());
        hadc1.Init.ScanConvMode          = (channel_count > 1) ? ENABLE : DISABLE;
        hadc1.Init.NbrOfConversion       = channel_count;
        hadc1.Init.ContinuousConvMode    = ENABLE;
        hadc1.Init.DMAContinuousRequests = ENABLE;
        HAL_ADC_Init(&hadc1);

        for (uint8_t rank = 0; rank < channel_count; ++rank) {
            ADC_ChannelConfTypeDef channel_config = {};
            channel_config.Channel                = ADC_HARDWARE_CHANNEL_MAP[channel_ids[rank]];
            channel_config.Rank                   = rank + 1;
            channel_config.SamplingTime = sampling_times[rank] < 8u ? sampling_times[rank] : 0u;
            HAL_ADC_ConfigChannel(&hadc1, &channel_config);
        }

        HAL_ADC_Start_DMA(&hadc1, buf_cast<uint32_t>(dma_buffer.data()), DMA_BUFFER_LENGTH);
    }

    void stop() override { HAL_ADC_Stop_DMA(&hadc1); }
};

IAdcHal& adc_hal()
{
    static AdcHal s_hal;
    return s_hal;
}

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void DMA2_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_adc1); }

extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef*)
{
    push_range(0, DMA_BUFFER_LENGTH / 2);
}

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef*)
{
    push_range(DMA_BUFFER_LENGTH / 2, DMA_BUFFER_LENGTH);
}

} // namespace kvasir
