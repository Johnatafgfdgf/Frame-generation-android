cmake_minimum_required(VERSION 3.22.1)
project(framegen_native LANGUAGES C CXX)

find_package(bytehook REQUIRED CONFIG)

find_library(log_lib log)
find_library(android_lib android)
find_library(vulkan_lib vulkan)

add_library(
        framegen_native
        SHARED
        native_probe.cpp
        vulkan_presenter.cpp
        vulkan_interceptor.cpp
)

target_link_libraries(
        framegen_native
        PRIVATE
        bytehook::bytehook
        ${log_lib}
        ${android_lib}
        ${vulkan_lib}
)

# Separate library that behaves like a tiny "guest" native library.
# The interceptor is installed before this library is loaded so ByteHook
# can prove that newly-loaded guest ELF files are intercepted.
add_library(
        framegen_guest_test
        SHARED
        guest_vulkan_test.cpp
)

target_link_libraries(
        framegen_guest_test
        PRIVATE
        ${log_lib}
        ${vulkan_lib}
)
