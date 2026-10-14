/*
 * Xbox speakers onto the host's: one fold-down for every path that has
 * 5.1 levels and a device with some other number of channels.
 *
 * The rules are the XAudio2 backend's (foldSpeaker in
 * audio_output_xaudio2.cpp), which is the reference: each Xbox speaker goes
 * to the same speaker when the device has it; else the centre to both fronts
 * and each back to its side or its front at -3 dB; the LFE is dropped; a mono
 * device takes everything but the LFE at -3 dB.
 *
 * Why it exists: SDL's own 5.1-to-stereo conversion scales the fronts by
 * 0.29 (-10.6 dB) to keep a full 5.1 signal from clipping, so a voice handed
 * to SDL as 5.1 played 10 dB quieter than one handed over as mono or stereo,
 * and quieter than the same voice on XAudio2. Folding before the device
 * keeps every path at one level; the limiter after the mix keeps it from
 * clipping.
 */
#pragma once

#include <stdint.h>

#include "audio_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Device layouts, in SDL's channel order (which is WAVEFORMATEXTENSIBLE's):
 *   1  mono
 *   2  FL FR
 *   3  FL FR LFE
 *   4  FL FR BL BR
 *   5  FL FR LFE BL BR
 *   6  FL FR FC LFE BL BR
 *   7  FL FR FC LFE BC SL SR       (BL/BR fold to the sides)
 *   8  FL FR FC LFE BL BR SL SR
 * Anything else is taken as stereo in its first two channels. */
#define RECOMP_FOLD_MAX_CHANNELS 8u

/* fold[s][o]: how much of Xbox speaker s (RecompSpeaker order) reaches
 * device channel o, for a device with `out_channels`. */
void recomp_audio_fold_matrix(uint32_t out_channels,
                              float fold[RECOMP_SPEAKERS][RECOMP_FOLD_MAX_CHANNELS]);

#ifdef __cplusplus
}
#endif
