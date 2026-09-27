function(nd_add_test target source name)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE nd_common)
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

nd_add_test(netdisplay-transport-test transport_test.c frame-transport)
nd_add_test(netdisplay-network-test network_test.c bidirectional-network-probe)
nd_add_test(netdisplay-crypto-test crypto_test.c crypto-roundtrip)

if(NETDISPLAY_BUILD_SERVER)
    nd_add_test(netdisplay-control-test control_test.c concurrent-control-sessions
        $<TARGET_FILE:netdisplay-server>)
    add_dependencies(netdisplay-control-test netdisplay-server)
endif()
