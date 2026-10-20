/*
 * Which path a title's sound takes (docs/technical/audio-architecture.md).
 *
 *   native  DirectSound is replaced by name (hle_dsound*.c) and each voice
 *           plays through a host voice. The default while the APU path is
 *           proven title by title.
 *   apu     The title's own DirectSound runs, unreplaced, and drives the
 *           emulated APU (src/apu); its voice processor's mix bins go
 *           through the speaker stage to one host stream.
 *
 * RECOMP_AUDIO=native|apu, or `audio = ...` in the settings file. Read once:
 * a title cannot change paths while it runs, since DirectSound's objects are
 * built by whichever path it started on.
 *
 * With neither set, a DirectSound older than 4134 starts on the APU path. The
 * native path reads buffer settings in the layout of 4134 and later, and an
 * older build keeps something else there, so none of its buffers is ever
 * modelled (hle_dsound.c, settings_in_layout): Tony Hawk's Pro Skater 3
 * (4039) had its music and not one sound effect, and on the APU path every
 * buffer it starts is heard.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_health.h"
#include "audio_path.h"

extern const char *recomp_config_lookup(const char *env_name, const char *key);
uint32_t hle_dsound_library_build(void);    /* hle_dsound.c; 0 when unknown */

enum { NATIVE_LAYOUT_FROM = 4134u };   /* the oldest build the native path reads */

static int g_apu = -1;

static int resolve(void)
{
    if (g_apu < 0) {
        const char *v = recomp_config_lookup("RECOMP_AUDIO", "audio");
        if (v && *v) {
            g_apu = strcmp(v, "apu") == 0 || strcmp(v, "APU") == 0;
        } else {
            uint32_t build = hle_dsound_library_build();
            g_apu = build != 0u && build < NATIVE_LAYOUT_FROM;
            if (g_apu)
                fprintf(stderr, "[AUDIO] DirectSound %u predates the buffer layout the "
                                "native path reads (%u): the APU path by default "
                                "(RECOMP_AUDIO=native for the other)\n",
                        build, NATIVE_LAYOUT_FROM);
        }
        recomp_audio_health_path(g_apu ? "apu" : "native");
    }
    return g_apu;
}

int recomp_audio_path_apu(void) { return resolve(); }

/* The switch the audio group's thunks read (tools/recomp/hle.py GROUPS). */
int recomp_hle_audio_native(void) { return !resolve(); }
