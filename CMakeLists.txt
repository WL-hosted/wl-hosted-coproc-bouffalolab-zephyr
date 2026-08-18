# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CherryUSB")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wlh_coproc_bouffalolab_zephyr LANGUAGES C)

# CherryUSB is built as a Zephyr module, so the descriptor and Bouffalo Lab
# compatibility headers must be visible through Zephyr's shared interface.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(core/coproc-core core-coproc-build EXCLUDE_FROM_ALL)
wlh_common_enable_zephyr_osal()

target_sources(app PRIVATE
    src/main.c
    src/transport_usb.c
    src/wifi_backend.c
)
target_include_directories(app PRIVATE include)
target_link_libraries(app PRIVATE wlh_coproc_core wlh::zephyr_osal)
target_compile_options(app PRIVATE -Wall -Werror)

# Core is ordinary portable CMake. Inherit Zephyr's target ABI flags (notably
# the BL616 single-precision floating-point ABI) on every portable archive.
foreach(wlh_target
        wlh_coproc_core
        wlh_protocol
        wlh_nanopb_runtime
        wlh_protocol_nanopb
        wlh_sim_nanopb)
    if(TARGET ${wlh_target})
        set_target_properties(${wlh_target} PROPERTIES
            POSITION_INDEPENDENT_CODE OFF)
        target_link_libraries(${wlh_target} PRIVATE zephyr_interface)
    endif()
endforeach()
