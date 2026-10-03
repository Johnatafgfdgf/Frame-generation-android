cmake_minimum_required(VERSION 3.22)
project(zalith_lsfg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED BANNER_CPP)
  message(FATAL_ERROR "BANNER_CPP is required")
endif()
if(NOT DEFINED VULKAN_INCLUDE)
  message(FATAL_ERROR "VULKAN_INCLUDE is required")
endif()
if(NOT DEFINED WINFG_SRC)
  message(FATAL_ERROR "WINFG_SRC is required")
endif()

add_subdirectory(${BANNER_CPP}/thirdparty/dxbc ${CMAKE_BINARY_DIR}/dxbc-build)

set(LSFG_DIR ${BANNER_CPP}/winlator/lsfg)

add_library(zalith_lsfg SHARED
  presenter_lsfg.cpp
  lsfg_vkd_standalone.cpp
  ${LSFG_DIR}/lsfg_dll.cpp
  ${LSFG_DIR}/lsfg_dxbc.cpp
  ${LSFG_DIR}/lsfg_common.cpp
  ${LSFG_DIR}/lsfg_shaders.cpp
  ${LSFG_DIR}/lsfg_mipmaps.cpp
  ${LSFG_DIR}/lsfg_alpha.cpp
  ${LSFG_DIR}/lsfg_beta.cpp
  ${LSFG_DIR}/lsfg_gamma.cpp
  ${LSFG_DIR}/lsfg_delta.cpp
  ${LSFG_DIR}/lsfg_generate.cpp
  ${LSFG_DIR}/lsfg_chain.cpp
  ${LSFG_DIR}/lsfg_pacer.cpp
  ${LSFG_DIR}/lsfg_engine.cpp
  ${LSFG_DIR}/lsfg_governor.cpp
)

target_include_directories(zalith_lsfg PRIVATE
  ${LSFG_DIR}
  ${VULKAN_INCLUDE}
  ${WINFG_SRC}
)

target_compile_definitions(zalith_lsfg PRIVATE
  VK_NO_PROTOTYPES=1
  VK_USE_PLATFORM_ANDROID_KHR
)

target_compile_options(zalith_lsfg PRIVATE -O2 -fvisibility=hidden)

target_link_libraries(zalith_lsfg
  dxbc
  android
  log
  dl
  atomic
)
