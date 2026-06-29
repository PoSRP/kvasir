cmake_minimum_required(VERSION 3.22)
project(capture)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(capture
    src/capture.cpp
    src/frame_parser.cpp
)
