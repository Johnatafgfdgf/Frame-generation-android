cmake_minimum_required(VERSION 3.22.1)
project(framegen_native LANGUAGES C CXX)

add_library(
        framegen_native
        SHARED
        native_probe.cpp
)

find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(
        framegen_native
        PRIVATE
        ${log-lib}
        ${vulkan-lib}
)
