cmake_minimum_required(VERSION 3.22)
project(unit-tests)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
)
FetchContent_MakeAvailable(Catch2)

add_executable(unit-tests
    test_ring_buffer.cpp
    test_frame.cpp
    test_pipeline.cpp
    test_frame_parser.cpp
    stubs/adc_stub.cpp
    stubs/led_stub.cpp
    stubs/usb_stub.cpp
    stubs/ymir_stub.cpp
    ../../firmware/app/app.cpp
    ../../firmware/app/frame.cpp
    ../../host/src/frame_parser.cpp
)

target_include_directories(unit-tests PRIVATE
    ../../firmware/app
    ../../firmware/ymir/firmware/include
    ../../host/src
    .
)

target_link_libraries(unit-tests PRIVATE Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(unit-tests)
