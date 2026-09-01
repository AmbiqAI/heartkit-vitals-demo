# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
cmake_minimum_required(VERSION 3.24)
project(heartkit-vitals-demo LANGUAGES C CXX ASM)

set(NSX_BOARD "apollo510b_evb" CACHE STRING "Target board")

# Reported verbatim in the HKV boot line so a captured SWO log is
# self-describing. Deliberately NOT derived from a git describe here: that
# would make the firmware identity depend on the state of the working tree at
# configure time and silently relabel a dirty build. A release build is
# expected to pass -DHKV_FW_VERSION=<tag>; anything else is honestly "dev".
set(HKV_FW_VERSION "dev" CACHE STRING "Firmware version reported in the HKV boot line")

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

add_executable(heartkit-vitals-demo
    src/main.cc
    src/obs.c
    src/sensor.c
    src/store.c
    src/metrics.c
    src/tflm.cc
    src/ecg_denoise.cc
    src/ecg_segmentation.cc
    src/ecg_arrhythmia.cc
    src/as7058_profiles.c
    src/ringbuffer.c
    src/stimulus.c
    src/nstdb_noise.c
    src/generated/as7058_profile_click_ppg_ecg.c
    src/generated/as7058_profile_click_spo2.c
)

# TileIO BLE is hardware-gated to apollo510b_evb (the only board in this
# app's family with the EM9305 BLE radio). ble_bringup.c's own contents are
# additionally guarded by `#if defined(AM_PART_APOLLO510B)` (see that file),
# but it must not even be compiled for the other boards -- apollo510_evb and
# apollo330mP_evb don't vendor nsx-cordio/nsx-ble/nsx-tileio-ble at all (see
# nsx.yml's `modules[].boards: [apollo510b_evb]` scoping), so tio_ble.h
# wouldn't even resolve for those targets.
if(NSX_BOARD STREQUAL "apollo510b_evb")
    target_sources(heartkit-vitals-demo PRIVATE src/ble_bringup.c)
endif()

target_include_directories(heartkit-vitals-demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

# Board name comes from the CMake variable that actually selected the build,
# not from an SoC preprocessor macro guessed in C -- the boot line should not
# be able to disagree with the build it came from.
target_compile_definitions(heartkit-vitals-demo PRIVATE
    HKV_BOARD_NAME="${NSX_BOARD}"
    HKV_FW_VERSION="${HKV_FW_VERSION}"
)

target_link_libraries(heartkit-vitals-demo PRIVATE
    nsx::board
    nsx::runtime_core
    nsx::freertos
    nsx::usb
    nsx::i2c
    nsx::spi
    # Linked for module/build coverage only. INA228 and LED-stick runtime
    # integration remains intentionally deferred.
    nsx::sensors
    nsx::gpio
    nsx::power
    nsx::as7058
    nsx::as7058_i2c
    nsx::as7058_spi
    nsx::physiokit
    nsx::helia_rt
    nsx::tileio_usb
)

if(NSX_BOARD STREQUAL "apollo510b_evb")
    target_link_libraries(heartkit-vitals-demo PRIVATE
        nsx::ble
        nsx::tileio_ble
    )
endif()

nsx_finalize_app(heartkit-vitals-demo)
