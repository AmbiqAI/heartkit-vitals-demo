# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
cmake_minimum_required(VERSION 3.24)
project(heartkit-vitals-demo LANGUAGES C CXX ASM)

set(NSX_BOARD "apollo510b_evb" CACHE STRING "Target board")

# __FILE__ in the vendored TFLM kernels otherwise bakes the builder's absolute
# checkout path into every shipped binary. Applied at directory scope, before
# nsx_bootstrap_app() runs its add_subdirectory() calls, so the module targets
# inherit it: the module trees are vendored by `nsx sync` and are not editable
# from this repo. Both values are resolved at configure time, so a hosted
# runner is covered without naming any host path. Rewrites `__FILE__` only:
# DWARF keeps absolute paths so a debugger still finds the sources. See #40.
add_compile_options(
    "-fmacro-prefix-map=${CMAKE_CURRENT_LIST_DIR}=."
    "-fmacro-prefix-map=${CMAKE_BINARY_DIR}=."
)

# Reported verbatim in the HKV boot line so a captured SWO log is
# self-describing. Deliberately NOT derived from a git describe here: that
# would make the firmware identity depend on the state of the working tree at
# configure time and silently relabel a dirty build. A release build is
# expected to pass -DHKV_FW_VERSION=<tag>; anything else is honestly "dev".
set(HKV_FW_VERSION "dev" CACHE STRING "Firmware version reported in the HKV boot line")

# OFF puts the AS7058 register reads back on the nsx-i2c blocking transfer,
# which spins in the sensor task. See src/sensor_bus.c and #65.
option(HKV_SENSOR_ASYNC "Read the AS7058 over the IOM command queue" ON)

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
    src/sensor_bus.c
    src/store.c
    src/metrics.c
    src/tflm.cc
    src/ecg_denoise.cc
    src/ecg_segmentation.cc
    src/ecg_arrhythmia.cc
    src/as7058_profiles.c
    src/ringbuffer.c
    src/stimulus.c
    src/timebase.c
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
#
# Both strings land unescaped in the boot line, whose fields are separated by
# spaces and whose records are separated by `|`. A value containing either
# breaks the format for a whitespace-splitting parser: -DHKV_FW_VERSION="1.2
# rc1" emits `fw=1.2 rc1 board=...` and silently shifts every later field by
# one. HKV_FW_VERSION is externally supplied, so validate rather than trust.
foreach(hkv_ident_var HKV_FW_VERSION NSX_BOARD)
    if("${${hkv_ident_var}}" MATCHES "[ 	|]")
        message(FATAL_ERROR
            "${hkv_ident_var}='${${hkv_ident_var}}' contains a space, tab or '|'. "
            "It is emitted verbatim into the HKV boot line, where those characters "
            "are field and record separators, so it would corrupt the line format.")
    endif()
endforeach()

target_compile_definitions(heartkit-vitals-demo PRIVATE
    HKV_BOARD_NAME="${NSX_BOARD}"
    HKV_FW_VERSION="${HKV_FW_VERSION}"
    HKV_SENSOR_ASYNC=$<BOOL:${HKV_SENSOR_ASYNC}>
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

# The heliaAOT model modules are generated and vendored ahead of the adapters
# that will call them, so nothing links them yet. Force them into the default
# build so a regeneration that no longer compiles fails here rather than in the
# PR that switches segmentation and arrhythmia over. See AmbiqAI/heartkit-vitals-demo#37.
# `nsx build` names the app target, not `all`, so the dependency hangs off the
# app rather than a custom ALL target.
add_dependencies(heartkit-vitals-demo hkv_segmentation_aot hkv_arrhythmia_aot)

nsx_finalize_app(heartkit-vitals-demo)
