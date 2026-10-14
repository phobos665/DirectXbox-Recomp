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
 */
#include <string.h>

#include "audio_health.h"
#include "audio_path.h"

extern const char *recomp_config_lookup(const char *env_name, const char *key);

static int g_apu = -1;

static int resolve(void)
{
    if (g_apu < 0) {
        const char *v = recomp_config_lookup("RECOMP_AUDIO", "audio");
        g_apu = v && (strcmp(v, "apu") == 0 || strcmp(v, "APU") == 0);
        recomp_audio_health_path(g_apu ? "apu" : "native");
    }
    return g_apu;
}

int recomp_audio_path_apu(void) { return resolve(); }

/* The switch the audio group's thunks read (tools/recomp/hle.py GROUPS). */
int recomp_hle_audio_native(void) { return !resolve(); }
