cmake_minimum_required(VERSION 3.16)
project(netdisplay LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(NETDISPLAY_BUILD_SERVER "Build netdisplay-server" ON)
option(NETDISPLAY_BUILD_CLIENT "Build netdisplay-client" ON)
set(NETDISPLAY_STATIC_CLIENT_PREFIX "" CACHE PATH
    "Optional prefix containing static libavcodec.a, libavutil.a and libdrm.a")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

add_library(nd_flags INTERFACE)
target_compile_options(nd_flags INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-O3;-Wall;-Wextra;-Wpedantic>)

if(NETDISPLAY_BUILD_SERVER)
    pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(SERVER_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
    pkg_check_modules(WLR REQUIRED wlr-protocols)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WLR_DATADIR wlr-protocols pkgdatadir)
    set(SC_XML "${WLR_DATADIR}/unstable/wlr-screencopy-unstable-v1.xml")

    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h"
        COMMAND "${WAYLAND_SCANNER}" client-header "${SC_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h"
        DEPENDS "${SC_XML}")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        COMMAND "${WAYLAND_SCANNER}" private-code "${SC_XML}"
                "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        DEPENDS "${SC_XML}")

    add_executable(netdisplay-server
        server.c video_sender.c
        "${CMAKE_CURRENT_BINARY_DIR}/screencopy.c"
        "${CMAKE_CURRENT_BINARY_DIR}/screencopy.h")
    target_include_directories(netdisplay-server PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(netdisplay-server PRIVATE
        nd_flags Threads::Threads PkgConfig::WAYLAND PkgConfig::SERVER_FFMPEG)
endif()

if(NETDISPLAY_BUILD_CLIENT)
    # VA stays dynamic so the client uses the receiver laptop's installed
    # libva + vendor VA driver.
    pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva libva-drm)
    add_executable(netdisplay-client client.c video_receiver.c)
    target_include_directories(netdisplay-client PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(netdisplay-client PRIVATE nd_flags Threads::Threads)

    if(NETDISPLAY_STATIC_CLIENT_PREFIX)
        find_path(ND_AV_INCLUDE libavcodec/avcodec.h
            PATHS "${NETDISPLAY_STATIC_CLIENT_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
        find_path(ND_DRM_INCLUDE xf86drm.h
            PATHS "${NETDISPLAY_STATIC_CLIENT_PREFIX}/include/libdrm"
                  "${NETDISPLAY_STATIC_CLIENT_PREFIX}/include"
            NO_DEFAULT_PATH REQUIRED)
        find_file(ND_AVCODEC_A libavcodec.a
            PATHS "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib"
                  "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib64"
            NO_DEFAULT_PATH REQUIRED)
        find_file(ND_AVUTIL_A libavutil.a
            PATHS "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib"
                  "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib64"
            NO_DEFAULT_PATH REQUIRED)
        find_file(ND_DRM_A libdrm.a
            PATHS "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib"
                  "${NETDISPLAY_STATIC_CLIENT_PREFIX}/lib64"
            NO_DEFAULT_PATH REQUIRED)
        target_include_directories(netdisplay-client PRIVATE "${ND_AV_INCLUDE}" "${ND_DRM_INCLUDE}")
        target_link_libraries(netdisplay-client PRIVATE
            "${ND_AVCODEC_A}" "${ND_AVUTIL_A}" "${ND_DRM_A}"
            PkgConfig::LIBVA m dl rt)
    else()
        # Used by the source fallback on an arbitrary receiver laptop.
        pkg_check_modules(CLIENT REQUIRED IMPORTED_TARGET libdrm libavcodec libavutil)
        target_link_libraries(netdisplay-client PRIVATE PkgConfig::CLIENT PkgConfig::LIBVA)
    endif()
endif()
