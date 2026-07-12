#ifndef KVASIR_HAL_MMIO_HPP
#define KVASIR_HAL_MMIO_HPP

#include "stm32f4xx_hal.h"
#include "main.h"
#include <cstdint>

// Centralize the NOLINT annotations for some typical ops

namespace kvasir {

// Volatile-typed pointer for scalar register access from an integer address.
template<typename T> [[nodiscard]] inline volatile T* mmio_ptr(uintptr_t addr)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    return reinterpret_cast<volatile T*>(addr);
}

// Non-volatile pointer to a CMSIS peripheral struct. The struct members carry
// their own __IO/volatile qualifier so the outer pointer must not be volatile.
template<typename T> [[nodiscard]] inline T* periph_ptr(uintptr_t addr)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    return reinterpret_cast<T*>(addr);
}

// Reinterpret between unrelated pointer types (e.g., handing a uint16_t DMA buffer to a HAL API
// that expects uint32_t*).
template<typename Dst, typename Src> [[nodiscard]] inline Dst* buf_cast(Src* p)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<Dst*>(p);
}

// Wrappers for opaque HAL/CubeMX macros whose expansions trip clang-tidy.

inline void enable_dma2_clock()
{
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    __HAL_RCC_DMA2_CLK_ENABLE();
}

inline void toggle_user_led()
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
    HAL_GPIO_TogglePin(USER_LED_GPIO_Port, USER_LED_Pin);
}

} // namespace kvasir

#endif // KVASIR_HAL_MMIO_HPP
