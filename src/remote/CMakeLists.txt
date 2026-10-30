# Remote play's picture (remote_stream.h): the host's frames as H.264 to the
# joiners, and the joiner itself, xboxrecomp-remote. Apart from xbox_video so
# the joiner, which holds no game code, does not pull the renderer in.
add_library(xbox_remote STATIC remote_stream.c)
target_include_directories(xbox_remote PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_remote PUBLIC xbox_input xbox_config SDL3::SDL3 ${CMAKE_DL_LIBS})

# FFmpeg is loaded at run time, as for XMV movies (src/video/CMakeLists.txt,
# whose search this repeats): only the headers are needed here. Without them
# remote play has controllers but no picture.
set(_ffmpeg "${XBOXRECOMP_FFMPEG_DIR}")
if(NOT _ffmpeg)
    set(_ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ffmpeg")
endif()
set(_ffmpeg_system OFF)
if(APPLE AND NOT EXISTS "${_ffmpeg}/include/libavcodec/avcodec.h")
    foreach(prefix /opt/homebrew /usr/local)
        if(NOT _ffmpeg_system AND EXISTS "${prefix}/include/libavcodec/avcodec.h")
            set(_ffmpeg "${prefix}")
            set(_ffmpeg_system ON)
        endif()
    endforeach()
endif()
if(EXISTS "${_ffmpeg}/include/libavcodec/avcodec.h")
    if(_ffmpeg_system)
        target_compile_options(xbox_remote PRIVATE -idirafter "${_ffmpeg}/include")
    else()
        target_include_directories(xbox_remote PRIVATE "${_ffmpeg}/include")
    endif()
    if(WIN32)
        set(_ffmpeg_bin "${_ffmpeg}/bin")
    else()
        set(_ffmpeg_bin "${_ffmpeg}/lib")
    endif()
    target_compile_definitions(xbox_remote PRIVATE REMOTE_HAVE_FFMPEG
        REMOTE_FFMPEG_BIN="${_ffmpeg_bin}")
    message(STATUS "xbox_remote: remote play's picture through FFmpeg at ${_ffmpeg}")
else()
    message(STATUS "xbox_remote: no FFmpeg headers; remote play has no picture")
endif()

if(MSVC)
    target_compile_options(xbox_remote PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_remote PRIVATE -Wall -Wextra -O2)
endif()

# Join a friend's game as controller 2-4. Holds no game code, so it can go to
# anyone; built with the toolkit, not with a title.
add_executable(xboxrecomp-remote remote_join.c)
target_link_libraries(xboxrecomp-remote PRIVATE xbox_remote)
