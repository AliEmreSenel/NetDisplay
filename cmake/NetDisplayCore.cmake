# Shared targets for the root build and standalone VR builds.
include_guard(GLOBAL)
get_filename_component(NETDISPLAY_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(NETDISPLAY_ENABLE_IPO "Enable interprocedural optimization" OFF)
if(NETDISPLAY_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ND_IPO_SUPPORTED OUTPUT ND_IPO_ERROR LANGUAGES C)
    if(NOT ND_IPO_SUPPORTED)
        message(FATAL_ERROR "IPO is unavailable: ${ND_IPO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET GLOBAL libsodium)

add_library(nd_flags INTERFACE)
target_compile_options(nd_flags INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall;-Wextra;-Wpedantic>)

function(nd_add_transport_library)
    if(TARGET nd_transport)
        return()
    endif()
    # This portable source set is compiled into the native iOS app as well.
    add_library(nd_protocol STATIC
        "${NETDISPLAY_SOURCE_DIR}/src/crypto.c"
        "${NETDISPLAY_SOURCE_DIR}/src/frame_receiver.c")
    target_include_directories(nd_protocol PUBLIC "${NETDISPLAY_SOURCE_DIR}/src")
    target_link_libraries(nd_protocol PUBLIC nd_flags PkgConfig::SODIUM)

    add_library(nd_transport STATIC
        "${NETDISPLAY_SOURCE_DIR}/src/frame_transport.c"
        "${NETDISPLAY_SOURCE_DIR}/src/session.c"
        "${NETDISPLAY_SOURCE_DIR}/src/network_test.c")
    target_link_libraries(nd_transport PUBLIC nd_protocol Threads::Threads)
endfunction()

function(nd_add_video_library)
    if(TARGET nd_video)
        return()
    endif()
    nd_add_transport_library()
    pkg_check_modules(ND_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL libavcodec libavutil libswscale)
    add_library(nd_video STATIC
        "${NETDISPLAY_SOURCE_DIR}/src/video_tx.c"
        "${NETDISPLAY_SOURCE_DIR}/src/encoder.c"
        "${NETDISPLAY_SOURCE_DIR}/src/encoder_rate.c")
    target_link_libraries(nd_video PUBLIC nd_transport PkgConfig::ND_FFMPEG)
endfunction()
