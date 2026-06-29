//
#include "interfaces/iadc.hpp"
#include "app.hpp"
#include "adc.h"
#include "stm32f4xx_hal.h"

//
#include <array>

static constexpr size_t DMA_BUF_LEN = 512;

static std::array<uint16_t, DMA_BUF_LEN> dma_buf;
static DMA_HandleTypeDef                 hdma_adc1;

static constexpr std::array<uint32_t, 9> adc_hw_ch_map = {
    ADC_CHANNEL_0, // ID 0 → PA0
    ADC_CHANNEL_1, // ID 1 → PA1
    ADC_CHANNEL_2, // ID 2 → PA2
    ADC_CHANNEL_3, // ID 3 → PA3
    ADC_CHANNEL_5, // ID 4 → PA5
    ADC_CHANNEL_6, // ID 5 → PA6
    ADC_CHANNEL_7, // ID 6 → PA7
    ADC_CHANNEL_8, // ID 7 → PB0
    ADC_CHANNEL_9, // ID 8 → PB1
};

static std::array<uint8_t, 9> s_scan_ids;
static uint8_t                s_scan_count = 1;

static void push_range(size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i)
        app_adc_push_ch(s_scan_ids[i % s_scan_count], dma_buf[i]);
}

class AdcHal final : public IAdcHal {
public:
    void start(std::span<const uint8_t> ch_ids, std::span<const uint8_t> sampling_times) override
    {
        if (ch_ids.empty()) {
            return;
        }

        s_scan_count = static_cast<uint8_t>(ch_ids.size());
        for (size_t i = 0; i < ch_ids.size(); ++i) {
            s_scan_ids[i] = ch_ids[i];
        }

        __HAL_RCC_DMA2_CLK_ENABLE();

        hdma_adc1.Instance                 = DMA2_Stream0;
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

        __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

        HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

        auto count                       = static_cast<uint8_t>(ch_ids.size());
        hadc1.Init.ScanConvMode          = (count > 1) ? ENABLE : DISABLE;
        hadc1.Init.NbrOfConversion       = count;
        hadc1.Init.ContinuousConvMode    = ENABLE;
        hadc1.Init.DMAContinuousRequests = ENABLE;
        HAL_ADC_Init(&hadc1);

        for (uint8_t rank = 0; rank < count; ++rank) {
            ADC_ChannelConfTypeDef sConfig = {};
            sConfig.Channel                = adc_hw_ch_map[ch_ids[rank]];
            sConfig.Rank                   = rank + 1;
            sConfig.SamplingTime           = sampling_times[rank] < 8u ? sampling_times[rank] : 0u;
            HAL_ADC_ConfigChannel(&hadc1, &sConfig);
        }

        HAL_ADC_Start_DMA(&hadc1, reinterpret_cast<uint32_t*>(dma_buf.data()), DMA_BUF_LEN);
    }

    void stop() override { HAL_ADC_Stop_DMA(&hadc1); }

    uint32_t compute_rate(std::span<const uint8_t> sampling_times) const override
    {
        if (sampling_times.empty())
            return 0u;
        uint32_t adcpre  = (ADC->CCR >> 16) & 0x3u;
        uint32_t adc_clk = HAL_RCC_GetPCLK2Freq() / (2u * (adcpre + 1u));
        static constexpr std::array<uint32_t, 8> cycles = {3, 15, 28, 56, 84, 112, 144, 480};
        uint32_t                                 total  = 0;
        for (uint8_t s : sampling_times)
            total += cycles[s < 8u ? s : 0u] + 12u;
        return adc_clk / total;
    }
};

IAdcHal& adc_hal()
{
    static AdcHal s_hal;
    return s_hal;
}

extern "C" void DMA2_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&hdma_adc1); }

extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef*) { push_range(0, DMA_BUF_LEN / 2); }

extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef*)
{
    push_range(DMA_BUF_LEN / 2, DMA_BUF_LEN);
}
