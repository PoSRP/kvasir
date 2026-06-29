include("${CMAKE_CURRENT_LIST_DIR}/../cubemx/cmake/gcc-arm-none-eabi.cmake")

# The CubeMX-generated toolchain hard-codes -T STM32F411XX_FLASH.ld into the
# linker flags. Strip it so the slot linker script chosen by the active preset
# (added via target_link_options in firmware/CMakeLists.txt) is the only one
# in effect — the app must land in the correct ymir slot, not at flash base.
string(REGEX REPLACE
    " -T \"[^\"]*STM32F411XX_FLASH.ld\""
    ""
    CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
