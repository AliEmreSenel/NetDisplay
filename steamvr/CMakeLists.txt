# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(netdisplay_steamvr LANGUAGES CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The NetDisplay SteamVR driver targets 64-bit Linux")
endif()
set(OPENVR_INCLUDE_DIR "" CACHE PATH "Directory containing Valve's openvr_driver.h")
if(NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr_driver.h")
    message(FATAL_ERROR "Set OPENVR_INCLUDE_DIR to the OpenVR SDK headers directory (see README.md)")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(Threads REQUIRED)
add_library(driver_netdisplay SHARED driver.cpp)
target_include_directories(driver_netdisplay SYSTEM PRIVATE "${OPENVR_INCLUDE_DIR}")
target_link_libraries(driver_netdisplay PRIVATE PkgConfig::SODIUM Threads::Threads)
target_compile_options(driver_netdisplay PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
set_target_properties(driver_netdisplay PROPERTIES PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/netdisplay/bin/linux64"
    BUILD_RPATH "$ORIGIN")
# SteamVR's container cannot necessarily see the host's libsodium installation.
# Resolve the actual SONAME dependency, rather than just the development symlink.
add_custom_command(TARGET driver_netdisplay POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DDRIVER=$<TARGET_FILE:driver_netdisplay>"
        "-DSODIUM_DIRS=${SODIUM_LIBRARY_DIRS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/bundle-sodium.cmake")
file(COPY driver.vrdrivermanifest DESTINATION "${CMAKE_BINARY_DIR}/netdisplay")
# Keep the user's pairing and display settings when CMake runs again.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/netdisplay/resources/settings/default.vrsettings")
    file(COPY resources DESTINATION "${CMAKE_BINARY_DIR}/netdisplay")
endif()
if(BUILD_TESTING)
    add_executable(motion-test motion_test.cpp)
    target_link_libraries(motion-test PRIVATE PkgConfig::SODIUM)
    add_test(NAME steamvr-motion COMMAND motion-test)
    add_executable(tracking-space-test tracking_space_test.cpp)
    add_test(NAME steamvr-stationary-space COMMAND tracking-space-test)
    add_executable(factory-test factory_test.cpp)
    target_include_directories(factory-test SYSTEM PRIVATE "${OPENVR_INCLUDE_DIR}")
    target_link_libraries(factory-test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME steamvr-factory COMMAND factory-test $<TARGET_FILE:driver_netdisplay>)
endif()
