function(nd_add_test target source name)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE nd_common)
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

add_executable(netdisplay-ios-wire-test ../ios/Native/NDNative.c ../ios/Tests/wire_test.c)
target_include_directories(netdisplay-ios-wire-test PRIVATE ../ios/Native)
target_link_libraries(netdisplay-ios-wire-test PRIVATE nd_protocol)
add_test(NAME ios-wire-compatibility COMMAND netdisplay-ios-wire-test)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME protocol-constants COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/generate-wire.py" --check)
endif()

if(TARGET nd_video AND NOT TARGET netdisplay-video-tx-test)
    include(../cmake/NetDisplayVideoTests.cmake)
endif()

nd_add_test(netdisplay-transport-test transport_test.c frame-transport)
nd_add_test(netdisplay-network-test network_test.c bidirectional-network-probe)
nd_add_test(netdisplay-crypto-test crypto_test.c crypto-roundtrip)
nd_add_test(netdisplay-session-test session_test.c session-authentication)
nd_add_test(netdisplay-power-test power_test.c power-telemetry)
nd_add_test(netdisplay-input-test input_test.c receiver-input)

if(NETDISPLAY_BUILD_SERVER)
    nd_add_test(netdisplay-control-test control_test.c concurrent-control-sessions
        $<TARGET_FILE:netdisplay-server>)
    add_dependencies(netdisplay-control-test netdisplay-server)
    set_tests_properties(concurrent-control-sessions PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME bandwidth-control-sessions COMMAND netdisplay-control-test
        $<TARGET_FILE:netdisplay-server> bandwidth)
    set_tests_properties(bandwidth-control-sessions PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_test(NAME all-intra-control-sessions COMMAND netdisplay-control-test
        $<TARGET_FILE:netdisplay-server> all-intra)
    set_tests_properties(all-intra-control-sessions PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()
