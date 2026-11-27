cmake_minimum_required(VERSION 3.20)
project(netdisplay LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
include(CTest)

include(cmake/NetDisplayCore.cmake)

option(NETDISPLAY_BUILD_SERVER "Build netdisplay-server" ON)
option(NETDISPLAY_BUILD_CLIENT "Build netdisplay-client" ON)
option(NETDISPLAY_BUILD_VR "Build the dedicated VR backend and capture layer" OFF)
option(NETDISPLAY_BUILD_STEAMVR "Build the standalone SteamVR driver" OFF)

# Linux device support is separate from the portable protocol and streaming core.
if(NETDISPLAY_BUILD_SERVER OR NETDISPLAY_BUILD_CLIENT OR BUILD_TESTING)
    nd_add_transport_library()
    add_library(nd_common STATIC
        src/power_state.c src/power_native.c src/input_receiver.c)
    target_link_libraries(nd_common PUBLIC nd_transport)
endif()

if(NETDISPLAY_BUILD_SERVER)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    nd_add_video_library()
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(SC_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml")
    set(OP_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h"
        COMMAND "${WAYLAND_SCANNER}" client-header "${SC_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h"
        DEPENDS "${SC_XML}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/output-power.h"
        COMMAND "${WAYLAND_SCANNER}" client-header "${OP_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/output-power.h"
        DEPENDS "${OP_XML}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/output-power.c"
        COMMAND "${WAYLAND_SCANNER}" private-code "${OP_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/output-power.c"
        DEPENDS "${OP_XML}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        COMMAND "${WAYLAND_SCANNER}" private-code "${SC_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        DEPENDS "${SC_XML}"
        VERBATIM)

    add_executable(netdisplay-server
        src/server.c src/display_state.c src/video_sender.c
        "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h"
        "${CMAKE_CURRENT_BINARY_DIR}/output-power.c"
        "${CMAKE_CURRENT_BINARY_DIR}/output-power.h")
    target_include_directories(netdisplay-server PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(netdisplay-server PRIVATE
        nd_common nd_video PkgConfig::WAYLAND)
endif()

if(NETDISPLAY_BUILD_CLIENT)
    pkg_check_modules(CLIENT REQUIRED IMPORTED_TARGET
        libdrm libavcodec libavutil libswscale libva libva-drm)
    add_executable(netdisplay-client src/client.c src/video_receiver.c)
    target_link_libraries(netdisplay-client PRIVATE
        nd_common PkgConfig::CLIENT ${CMAKE_DL_LIBS})
endif()

if(NETDISPLAY_BUILD_STEAMVR)
    add_subdirectory(steamvr)
endif()
if(NETDISPLAY_BUILD_VR)
    add_subdirectory(vr)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
