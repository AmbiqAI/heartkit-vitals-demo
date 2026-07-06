# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
cmake_minimum_required(VERSION 3.24)
project(nsx-port LANGUAGES C CXX ASM)

set(NSX_BOARD "apollo510b_evb" CACHE STRING "Target board")

include(${CMAKE_CURRENT_LIST_DIR}/cmake/nsx/modules.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/nsx/nsx_app_bootstrap.cmake)

# FreeRTOSConfig.h is application policy, not SDK policy. The nsx-freertos
# module consumes it through the nsx::freertos_config interface contract, so
# the app must publish that target BEFORE the bootstrap adds nsx-freertos.
add_library(app_freertos_config INTERFACE)
target_include_directories(app_freertos_config INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/config
)
add_library(nsx::freertos_config ALIAS app_freertos_config)

nsx_bootstrap_app(
    APP_ROOT "${CMAKE_CURRENT_LIST_DIR}"
    BOARD "${NSX_BOARD}"
    MODULES ${NSX_APP_MODULES}
)

add_executable(nsx-port
    src/main.cc
    src/sensor.c
    src/store.c
    src/metrics.c
    src/tflm.cc
    src/ecg_denoise.cc
    src/ecg_segmentation.cc
    src/ecg_arrhythmia.cc
    src/as7058_profiles.c
    src/ringbuffer.c
    src/generated/as7058_profile_click_ppg_ecg.c
    src/generated/as7058_profile_click_spo2.c
)

target_include_directories(nsx-port PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(nsx-port PRIVATE
    nsx::board
    nsx::runtime_core
    nsx::freertos
    nsx::usb
    nsx::i2c
    nsx::spi
    nsx::gpio
    nsx::power
    nsx::as7058
    nsx::as7058_i2c
    nsx::as7058_spi
    nsx::physiokit
    nsx::helia_rt
    nsx::tileio_usb
)

nsx_finalize_app(nsx-port)
