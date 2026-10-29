# Reading an Xbox disk image: a made-up FATX volume (folders, a file over
# several clusters, a deleted entry), raw and wrapped in qcow2 v3, copied
# out and read back. Portable; no console's data.
add_executable(test_xbox_hdd test_xbox_hdd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hdd/xbox_hdd.c)
target_include_directories(test_xbox_hdd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hdd)
add_test(NAME xbox_hdd COMMAND test_xbox_hdd ${CMAKE_CURRENT_BINARY_DIR}/xbox_hdd_test)
