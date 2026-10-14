# audio_output -- the SDL3 host audio output (src/hle/audio_output_sdl.c)
# keeps audio_output.h's contract: a muted device still clocks, 64 in flight,
# a refused chunk is taken later; and the native path's room hears a voice's
# send in step with the voice. Always muted (RECOMP_MUTE=1 forced). Each
# prints SKIP and passes when the machine has no audio device.

set(AUDIO_OUTPUT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_output_sdl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_speakers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_reverb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_health.c
)

foreach(t audio_output_clock audio_output_reverb)
    add_executable(${t} ${t}.c ${AUDIO_OUTPUT_SOURCES})
    target_include_directories(${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
    target_link_libraries(${t} PRIVATE SDL3::SDL3 platform m)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES ENVIRONMENT "RECOMP_MUTE=1")
endforeach()
