cmake_minimum_required(VERSION 3.22)
project(zalith_winfg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED WINFG_SRC)
  message(FATAL_ERROR "WINFG_SRC is required")
endif()
if(NOT DEFINED VULKAN_INCLUDE)
  message(FATAL_ERROR "VULKAN_INCLUDE is required")
endif()

add_library(zalith_winfg SHARED
  presenter.cpp
  ${WINFG_SRC}/framegen.cpp
)

target_include_directories(zalith_winfg PRIVATE
  ${WINFG_SRC}
  ${VULKAN_INCLUDE}
)

target_compile_definitions(zalith_winfg PRIVATE
  VK_NO_PROTOTYPES=1
  VK_USE_PLATFORM_ANDROID_KHR
)

target_link_libraries(zalith_winfg
  android
  log
  dl
)
