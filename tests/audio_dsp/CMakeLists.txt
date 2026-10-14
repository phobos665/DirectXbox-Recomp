# The audio path's plain-C stages -- the speaker fold-down and the I3DL2
# reverb -- with no device and no game. Portable.
add_executable(test_audio_dsp
    test_audio_dsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_speakers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle/audio_reverb.c
)
target_include_directories(test_audio_dsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/hle
)
if(MSVC)
    target_compile_definitions(test_audio_dsp PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
    target_link_libraries(test_audio_dsp PRIVATE m)
endif()
add_test(NAME audio_dsp COMMAND test_audio_dsp)
