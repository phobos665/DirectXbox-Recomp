/* Which path a title's sound takes: see audio_path.c. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when the title's own DirectSound drives the emulated APU
 * (RECOMP_AUDIO=apu); zero on the native DirectSound path. */
int recomp_audio_path_apu(void);

/* The audio HLE group's switch: nonzero while DirectSound is replaced. */
int recomp_hle_audio_native(void);

#ifdef __cplusplus
}
#endif
