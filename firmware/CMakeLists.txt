cmake_minimum_required(VERSION 3.22)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(kvasir)

enable_language(C CXX ASM)


if(NOT DEFINED SLOT_LINKER_SCRIPT)
    message(FATAL_ERROR
        "SLOT_LINKER_SCRIPT must be set via a CMake preset (slot-a or slot-b). "
        "The firmware is built as an application image for the ymir bootloader.")
endif()

add_executable(${CMAKE_PROJECT_NAME})

# Override the cubemx default linker script with the ymir slot layout.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    "-T${SLOT_LINKER_SCRIPT}"
)

# Pull in HAL, USB middleware, and CubeMX application scaffolding.
add_subdirectory(cubemx/cmake/stm32cubemx)

# Bring in the ymir bootloader
add_subdirectory(ymir/firmware/ymir_api ${CMAKE_BINARY_DIR}/ymir_api)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    app/app.cpp
    app/frame.cpp
    app/hal/adc_hal.cpp
    app/hal/led_hal.cpp
    app/hal/usb_cdc_hal.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
)

target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx ymir_api)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
        $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
)
