cmake_minimum_required(VERSION 3.22.1)
project(framegen_native LANGUAGES C CXX)

add_library(
        framegen_native
        SHARED
        native_probe.cpp
        vulkan_presenter.cpp
)

find_library(log_lib log)
find_library(android_lib android)
find_library(vulkan_lib vulkan)

target_link_libraries(
        framegen_native
        PRIVATE
        ${log_lib}
        ${android_lib}
        ${vulkan_lib}
)
