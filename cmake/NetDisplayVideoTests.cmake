include_guard(GLOBAL)
# Test the shared worker with a deterministic transport double and real AVPackets.
add_executable(netdisplay-video-tx-test
    "${NETDISPLAY_SOURCE_DIR}/tests/video_tx_test.c"
    "${NETDISPLAY_SOURCE_DIR}/src/video_tx.c")
target_include_directories(netdisplay-video-tx-test PRIVATE "${NETDISPLAY_SOURCE_DIR}/src")
target_link_libraries(netdisplay-video-tx-test PRIVATE
    nd_flags PkgConfig::ND_FFMPEG PkgConfig::SODIUM Threads::Threads)
target_compile_options(netdisplay-video-tx-test PRIVATE -UNDEBUG)
add_test(NAME video-transmit-policy COMMAND netdisplay-video-tx-test)
set_tests_properties(video-transmit-policy PROPERTIES TIMEOUT 10)

add_executable(netdisplay-stream-test
    "${NETDISPLAY_SOURCE_DIR}/tests/stream_test.c"
    "${NETDISPLAY_SOURCE_DIR}/ios/Native/NDNative.c")
target_include_directories(netdisplay-stream-test PRIVATE "${NETDISPLAY_SOURCE_DIR}/ios/Native")
target_link_libraries(netdisplay-stream-test PRIVATE nd_video)
add_test(NAME integrated-video-stream COMMAND netdisplay-stream-test)
set_tests_properties(integrated-video-stream PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
