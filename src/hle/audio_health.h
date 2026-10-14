/*
 * Whether a run's sound was heard: one line a run's log can be judged by.
 *
 * Every path that hands sound to the host reports here: the native
 * DirectSound path through both host backends (audio_output_*), and the APU
 * path from its voice processor. Every ten seconds, and at shutdown:
 *
 *   [AUDIO] health t=60s path=native: buffers used 14 heard 13 silent 1;
 *           streams used 1 heard 1 silent 0; refused 0
 *
 * "used" is a voice that was given sound at least once, "heard" one that was
 * given at least one non-silent sample, "silent" the rest. Some silence is
 * the title's own: TimeSplitters 2 loops four 64-byte buffers of zeros from
 * start-up, so its runs show four silent buffers whatever the path. A title
 * whose effects all play silent shows buffers heard 0 -- which is how
 * TimeSplitters 2 sounded after the envelope swap, with every scripted run
 * passing. scripts/regress.py
 * and run_and_report.py read the last line.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which path is producing the sound: "native" (DirectSound replaced by
 * name) or "apu" (the title's own DirectSound on the emulated APU). */
void recomp_audio_health_path(const char *path);

/* A voice was given sound. `stream` says whether it is a stream (music,
 * movies) or a buffer (effects); `nonzero` whether any sample in it was
 * not silence. `voice` is the backend slot, or the APU voice handle;
 * 0..511. */
void recomp_audio_health_sound(uint32_t voice, int stream, int nonzero);

/* Sound offered to the host and refused (the backend's queue was full, the
 * voice could not be made, the PCM was malformed). */
void recomp_audio_health_refused(void);

/* Print the line now (shutdown); otherwise it prints itself every 10 s
 * from recomp_audio_health_sound. */
void recomp_audio_health_report(void);

#ifdef __cplusplus
}
#endif
