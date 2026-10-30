# Remote play's host on 127.0.0.1, driven by raw datagrams: joining, the port
# handed out, newest-wins pad state, codes, a full game, rumble, leaving.
# No game, no window, no pad needed.
add_executable(test_remote_pads test_remote_pads.c)
target_link_libraries(test_remote_pads PRIVATE xbox_input)
add_test(NAME remote_pads COMMAND test_remote_pads)
