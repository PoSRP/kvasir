#include "mmio.hpp"
#include "stm32f4xx_hal.h"

namespace kvasir {

extern "C" void led_tick_1hz()
{
    // A simple 1Hz blink
    static uint32_t last = 0;
    const uint32_t  now  = HAL_GetTick();
    if (now - last >= 500) {
        toggle_user_led();
        last = now;
    }
}

} // namespace kvasir
