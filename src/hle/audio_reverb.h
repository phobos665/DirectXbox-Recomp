/*
 * An I3DL2 room reverb, for a backend that has none of its own (SDL).
 *
 * The console's reverb is the GP's I3DL2 effect, fed by mix bin 10 and set
 * with IDirectSound_SetI3DL2Listener; XAudio2 has one that takes the same
 * parameters. This is the same shape in plain C: the room's high-frequency
 * cut, a pre-delay, early reflections at reflections_delay, and a late tail
 * from an eight-line feedback delay network whose decay is decay_time, with
 * high frequencies dying at decay_hf_ratio of it, starting reverb_delay
 * after the reflections. Diffusion sets the input allpasses, density the
 * line lengths. A mono send in, stereo out.
 */
#pragma once

#include <stdint.h>

#include "audio_output.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompReverb RecompReverb;

RecompReverb *recomp_reverb_create(uint32_t rate);
void recomp_reverb_destroy(RecompReverb *r);

/* The room; NULL, or a room level of -10000 and below, is silence. Takes
 * effect from the next sample, the tail of the old room ringing on. */
void recomp_reverb_set_room(RecompReverb *r, const RecompI3dl2 *room);

/* Nonzero while there is a room to hear. */
int recomp_reverb_active(const RecompReverb *r);

/* Runs `count` samples of the send `in` (stride `in_stride` floats) and adds
 * the wet stereo into `out_l` / `out_r` (stride `out_stride`). */
void recomp_reverb_process(RecompReverb *r, const float *in, uint32_t in_stride,
                           float *out_l, float *out_r, uint32_t out_stride,
                           uint32_t count);

#ifdef __cplusplus
}
#endif
