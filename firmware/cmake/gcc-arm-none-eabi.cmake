include("${CMAKE_CURRENT_LIST_DIR}/../cubemx/cmake/gcc-arm-none-eabi.cmake")

# The CubeMX-generated toolchain hard-codes -T STM32F411XX_FLASH.ld into the
# linker flags. Strip it so the slot linker script chosen by the active preset.
string(REGEX REPLACE
    " -T \"[^\"]*STM32F411XX_FLASH.ld\""
    ""
    CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
