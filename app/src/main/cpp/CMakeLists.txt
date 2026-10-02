cmake_minimum_required(VERSION 3.22.1)
project(framegen_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(bytehook REQUIRED CONFIG)

find_library(log_lib log)
find_library(android_lib android)
find_library(vulkan_lib vulkan)

# -----------------------------------------------------------------------------
# Host runtime / guest interceptor
# -----------------------------------------------------------------------------
add_library(
        framegen_native
        SHARED
        native_probe.cpp
        vulkan_presenter.cpp
        vulkan_interceptor.cpp
        frame_bridge.cpp
        guest_frame_bridge.cpp
)

target_compile_definitions(
        framegen_native
        PRIVATE
        VK_USE_PLATFORM_ANDROID_KHR=1
)

target_link_libraries(
        framegen_native
        PRIVATE
        bytehook::bytehook
        ${log_lib}
        ${android_lib}
        ${vulkan_lib}
)

# -----------------------------------------------------------------------------
# Controlled Vulkan guest test
# -----------------------------------------------------------------------------
add_library(
        framegen_guest_test
        SHARED
        guest_vulkan_test.cpp
)

target_compile_definitions(
        framegen_guest_test
        PRIVATE
        VK_USE_PLATFORM_ANDROID_KHR=1
)

target_link_libraries(
        framegen_guest_test
        PRIVATE
        ${log_lib}
        ${android_lib}
        ${vulkan_lib}
)

# -----------------------------------------------------------------------------
# Lossless Scaling Vulkan backend
#
# Kept in a separate shared object because framegen uses volk and calls
# volkLoadDevice(). The host compositor/interceptor intentionally uses Android's
# system Vulkan loader directly. Keeping them in separate ELF namespaces avoids
# letting framegen's global volk dispatch table become the host dispatch table.
# -----------------------------------------------------------------------------
set(
        LSFG_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/lsfg-vk-android"
)

if(NOT EXISTS "${LSFG_ROOT}/framegen/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing third_party/lsfg-vk-android. Clone git submodules recursively.")
endif()

if(NOT EXISTS "${LSFG_ROOT}/thirdparty/volk/volk.c")
    message(FATAL_ERROR
        "Missing lsfg-vk nested volk submodule. Clone recursively.")
endif()

add_library(
        volk
        STATIC
        "${LSFG_ROOT}/thirdparty/volk/volk.c"
)

target_include_directories(
        volk
        PUBLIC
        "${LSFG_ROOT}/thirdparty/volk"
)

target_compile_definitions(
        volk
        PUBLIC
        VK_USE_PLATFORM_ANDROID_KHR=1
)

add_subdirectory(
        "${LSFG_ROOT}/framegen"
        "${CMAKE_BINARY_DIR}/lsfg-framegen"
        EXCLUDE_FROM_ALL
)

add_library(
        lossless_backend
        SHARED
        lossless_backend.cpp
)

target_compile_definitions(
        lossless_backend
        PRIVATE
        VK_USE_PLATFORM_ANDROID_KHR=1
)

target_include_directories(
        lossless_backend
        PRIVATE
        "${LSFG_ROOT}/framegen/public"
)

target_link_libraries(
        lossless_backend
        PRIVATE
        lsfg-vk-framegen
        volk
        ${log_lib}
        ${android_lib}
)
