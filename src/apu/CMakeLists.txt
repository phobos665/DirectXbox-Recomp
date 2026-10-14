# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

# libsamplerate (third_party/libsamplerate, 0.2.2, BSD-2): the voice
# processor resamples every voice through it. Built from its sources with a
# fixed config (samplerate_config/config.h) rather than its own CMake project.
set(XBOXRECOMP_SAMPLERATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libsamplerate)
add_library(xbox_samplerate STATIC
    ${XBOXRECOMP_SAMPLERATE_DIR}/src/samplerate.c
    ${XBOXRECOMP_SAMPLERATE_DIR}/src/src_linear.c
    ${XBOXRECOMP_SAMPLERATE_DIR}/src/src_sinc.c
    ${XBOXRECOMP_SAMPLERATE_DIR}/src/src_zoh.c
)
target_include_directories(xbox_samplerate
    PUBLIC  ${XBOXRECOMP_SAMPLERATE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/samplerate_config)
target_compile_definitions(xbox_samplerate PRIVATE HAVE_CONFIG_H)
if(MSVC)
    target_compile_definitions(xbox_samplerate PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(xbox_samplerate PRIVATE /wd4244 /wd4267)
endif()

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp_xemu.c   # xemu's vp.c, see its header for the changes
    apu_dsp.c
    apu_mmio_hook.c
    apu_fault.c
    apu_xaudio2.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# xemu's APU headers, by their xemu paths (hw/xbox/mcpx/apu/...): the shim
# directory first, so QEMU's headers and apu_int.h are answered by
# xemu_shim, then xemu itself for vp/vp.h, svf.h, hrtf.h, adpcm.h.
target_include_directories(xbox_apu BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/xemu_shim
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xemu
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/xemu/hw/xbox/mcpx/apu/vp  # adpcm.h, beside vp.c in xemu
)
target_link_libraries(xbox_apu PUBLIC xbox_samplerate)

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL3 audio + libm
    target_link_libraries(xbox_apu PRIVATE platform SDL3::SDL3 m)
endif()
