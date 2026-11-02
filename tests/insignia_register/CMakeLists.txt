# Registering a console with Insignia: the request body, what each answer
# means, and one request through the real transport (WinHTTP, or libcurl
# loaded at run time) to a stand-in server on 127.0.0.1. Never reaches
# insignia.live. Portable.
add_executable(test_insignia_register test_insignia_register.c)
target_link_libraries(test_insignia_register PRIVATE xbox_net)
if(WIN32)
    target_link_libraries(test_insignia_register PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(test_insignia_register PRIVATE Threads::Threads)
endif()
add_test(NAME insignia_register
         COMMAND test_insignia_register ${CMAKE_CURRENT_BINARY_DIR})
