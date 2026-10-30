# Remote play's picture without a game: encode, split into datagram pieces,
# reassemble, decode, compare; then a lost piece and the keyframe that ends
# it. Passes, saying so, where there is no FFmpeg to test with.
add_executable(test_remote_stream test_remote_stream.c)
target_link_libraries(test_remote_stream PRIVATE xbox_remote)
add_test(NAME remote_stream COMMAND test_remote_stream)
