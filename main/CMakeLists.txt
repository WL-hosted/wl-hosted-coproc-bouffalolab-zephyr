# SPDX-License-Identifier: Apache-2.0

# CherryUSB is built as a Zephyr module, so the descriptor and Bouffalo Lab
# compatibility headers must be visible through Zephyr's shared interface.
zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/cherryusb/include
)

target_sources(app PRIVATE
    app/main.c
)
target_sources_ifdef(CONFIG_WLH_USB_TRANSPORT app PRIVATE
    transports/usb/bflb_usb_platform.c
    transports/usb/transport_usb.c
)
if(CONFIG_WLH_WIFI_BACKEND AND CONFIG_WLH_USB_TRANSPORT)
    target_sources(app PRIVATE backends/wifi_backend.c)
endif()
target_include_directories(app PRIVATE
    app
    backends
    transports
    transports/usb
)
target_compile_options(app PRIVATE -Wall -Werror)

# Platform-agnostic Coprocessor Core (plain CMake static library). Protocol
# and Common are nested submodules of coproc-core; tests stay disabled in the
# Zephyr build.
if(CONFIG_WLH_USB_TRANSPORT)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/coproc-core
        ${CMAKE_BINARY_DIR}/core-coproc-build
        EXCLUDE_FROM_ALL
    )
    wlh_common_enable_zephyr_osal()
    target_link_libraries(app PRIVATE wlh_coproc_core wlh::zephyr_osal)
endif()

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
