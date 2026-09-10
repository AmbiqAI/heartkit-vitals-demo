# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
# Standalone host test project for heartkit-vitals-demo.
#
# This is deliberately independent of the NSX bootstrap: `nsx` has no test
# subcommand and no host build mode, so host tests are built directly with
# CMake using the host compiler (no toolchain file, no cross flags).
#
#   cmake -S tests -B build/host
#   cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(hkv_host_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HKV_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# -fno-sanitize-recover=all is required: without it UBSan prints a diagnostic
# and CONTINUES, the process still exits 0, and ctest reports PASS. The
# sanitizers only gate the build if they abort.
set(HKV_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all)
set(HKV_TEST_FLAGS -Wall -Wextra -Werror -g ${HKV_SAN_FLAGS})

enable_testing()

foreach(part APOLLO510B APOLLO510 APOLLO330P)
    set(test_name test_battery_${part})
    add_executable(${test_name} test_battery_model.c)
    target_include_directories(${test_name} PRIVATE ${HKV_SRC_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${test_name} PRIVATE AM_PART_${part})
    target_compile_options(${test_name} PRIVATE ${HKV_TEST_FLAGS})
    target_link_options(${test_name} PRIVATE ${HKV_SAN_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Unit under test. Compiled per test binary so each test gets a clean copy.
#
# Linking ringbuffer.c into every test is harmless for the ones that do not
# call it (test_obs_format exercises the header-only src/obs_fmt.h, which is
# deliberately dependency-free so it CAN be tested on the host -- the rest of
# src/obs.h needs FreeRTOS and nsx and cannot).
set(HKV_TEST_SOURCES
    test_ringbuffer.c
    test_ringbuffer_overwrite.c
    test_ringbuffer_model.c
    test_obs_format.c
    test_ecg_tensor_copy.c
    test_tio_tx_sm.c
    test_telemetry.c
)

foreach(test_src ${HKV_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src} ${HKV_SRC_DIR}/ringbuffer.c)
    target_include_directories(${test_name} PRIVATE ${HKV_SRC_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${test_name} PRIVATE ${HKV_TEST_FLAGS})
    target_link_options(${test_name} PRIVATE ${HKV_SAN_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_executable(test_denoise_adapter test_denoise_adapter.cc ${HKV_SRC_DIR}/ecg_denoise.cc)
target_include_directories(test_denoise_adapter PRIVATE fakes ${HKV_SRC_DIR})
target_compile_options(test_denoise_adapter PRIVATE ${HKV_TEST_FLAGS})
target_link_options(test_denoise_adapter PRIVATE ${HKV_SAN_FLAGS})
add_test(NAME test_denoise_adapter COMMAND test_denoise_adapter)
