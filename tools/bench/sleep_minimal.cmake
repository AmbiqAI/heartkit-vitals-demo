# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
if(NOT NSX_TOOLCHAIN_FAMILY STREQUAL "gcc")
    message(FATAL_ERROR "hkv_sleep_minimal requires the GCC linker")
endif()
option(HKV_SLEEP_SINGLE_MRAM "Power only MRAM bank zero in sleep test" OFF)
option(HKV_SLEEP_MRAM_LOW_POWER_READ "Use SDK MRAM low-power read policy in sleep test" OFF)
set(HKV_SLEEP_TCM_KIB 160 CACHE STRING "Sleep test total TCM: 160, 384, 768 KiB")
set(HKV_SLEEP_SRAM_MIB 0 CACHE STRING "Sleep test shared SRAM: 0, 1, 2, 3 MiB")
if(NOT HKV_SLEEP_TCM_KIB MATCHES "^(160|384|768)$" OR
   NOT HKV_SLEEP_SRAM_MIB MATCHES "^[0-3]$")
    message(FATAL_ERROR "Unsupported sleep test memory configuration")
endif()

set(_sleep_flags "nsx_board_${NSX_BOARD}_flags")
get_target_property(_sleep_options ${_sleep_flags} INTERFACE_LINK_OPTIONS)
set(_sleep_script_count 0)
foreach(_option IN LISTS _sleep_options)
    if(_option MATCHES "^-T(.+)$")
        set(_sleep_source "${CMAKE_MATCH_1}")
        math(EXPR _sleep_script_count "${_sleep_script_count} + 1")
    endif()
endforeach()
if(NOT _sleep_script_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one SDK linker script for sleep test")
endif()

# Keep startup sections in sync with the SDK, but bound all allocatable RAM;
# the SDK heap otherwise extends beyond the powered TCM banks. See #68.
file(READ "${_sleep_source}" _sleep_ld)
foreach(_region IN ITEMS MCU_ITCM MCU_TCM SHARED_SRAM)
    if(_region STREQUAL "MCU_ITCM")
        set(_length 32768)
    elseif(_region STREQUAL "MCU_TCM")
        set(_length 131072)
    else()
        set(_length 0)
    endif()
    set(_pattern "(${_region}[ \t]+\\(rwx\\)[ \t]*:[ \t]*ORIGIN[ \t]*=[ \t]*0x[0-9a-fA-F]+,[ \t]*LENGTH[ \t]*=[ \t]*)[0-9]+")
    if(NOT _sleep_ld MATCHES "${_pattern}")
        message(FATAL_ERROR "Cannot restrict ${_region} in SDK linker script")
    endif()
    string(REGEX REPLACE "${_pattern}" "\\1${_length}" _sleep_ld "${_sleep_ld}")
endforeach()
set(_mram_pattern "(MCU_MRAM[ \t]+\\(rx\\)[ \t]*:[ \t]*ORIGIN[ \t]*=[ \t]*0x00410000,[ \t]*LENGTH[ \t]*=[ \t]*)[0-9]+")
if(NOT _sleep_ld MATCHES "${_mram_pattern}")
    message(FATAL_ERROR "Cannot restrict sleep image to MRAM bank zero")
endif()
# The SBL reservation precedes the application inside the first MRAM bank.
# Bank size comes from am_hal_mram.h, AM_HAL_MRAM_INSTANCE_SIZE. See #68.
string(REGEX REPLACE "${_mram_pattern}" "\\12031616" _sleep_ld "${_sleep_ld}")
set(_sleep_script "${CMAKE_CURRENT_BINARY_DIR}/hkv_sleep_minimal.ld")
file(WRITE "${_sleep_script}" "${_sleep_ld}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sleep_source}")

# The SDK overlay helper changes every app's script. Scope this replacement
# to the sleep executable so production memory placement is unchanged. See #68.
list(REMOVE_ITEM _sleep_options "-T${_sleep_source}")
list(APPEND _sleep_options
    "-T$<IF:$<STREQUAL:$<TARGET_PROPERTY:NAME>,hkv_sleep_minimal>,${_sleep_script},${_sleep_source}>")
set_property(TARGET ${_sleep_flags} PROPERTY INTERFACE_LINK_OPTIONS ${_sleep_options})

add_executable(hkv_sleep_minimal tools/bench/sleep_minimal.c)
target_compile_definitions(hkv_sleep_minimal PRIVATE
    HKV_SLEEP_TCM_KIB=${HKV_SLEEP_TCM_KIB}
    HKV_SLEEP_SRAM_MIB=${HKV_SLEEP_SRAM_MIB}
    HKV_SLEEP_SINGLE_MRAM=$<BOOL:${HKV_SLEEP_SINGLE_MRAM}>
    HKV_SLEEP_MRAM_LOW_POWER_READ=$<BOOL:${HKV_SLEEP_MRAM_LOW_POWER_READ}>)
target_link_libraries(hkv_sleep_minimal PRIVATE nsx::board nsx::runtime_core nsx::power)
set_property(TARGET hkv_sleep_minimal APPEND PROPERTY LINK_DEPENDS "${_sleep_script}")
nsx_finalize_app(hkv_sleep_minimal)
