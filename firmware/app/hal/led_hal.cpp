#include "main.h"
#include "stm32f4xx_hal.h"

// Toggle the user LED at 1 Hz. Visible heartbeat that distinguishes a running
// kvasir app from the bootloader's double-blink DFU pattern.
extern "C" void led_tick_1hz()
{
    static uint32_t last = 0;
    uint32_t        now  = HAL_GetTick();
    if (now - last >= 500) {
        HAL_GPIO_TogglePin(USER_LED_GPIO_Port, USER_LED_Pin);
        last = now;
    }
}
