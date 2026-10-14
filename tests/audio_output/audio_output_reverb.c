/*
 * audio_output_reverb.c -- the SDL backend's room for the native path: a
 * voice's reverb send reaches the room in step with the voice, and the room
 * answers (src/hle/audio_output_sdl.c, "The native path's room").
 *
 * RECOMP_MUTE=1 is forced: the bus silences its wet sound after the room has
 * run, so its counters still see it. Checks:
 *   1. with a room and a voice that sends to it, a click submitted behind
 *      300 ms of silence reaches the room no sooner than the voice plays it
 *      -- a send summed when it was submitted would arrive at once;
 *   2. the room answers: wet sound, starting no sooner than the send, and
 *      still ringing after the click has passed;
 *   3. with the room cleared, nothing more comes out of it.
 * Needs an audio device; prints SKIP and exits 0 when there is none.
 */
#include "audio_output.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void recomp_audio_output_reverb_stats(uint64_t *send_frames, uint64_t *wet_frames,
                                      uint64_t *first_send_ns, uint64_t *first_wet_ns);

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    enum { RATE = 22050, SLOT = 5, SILENCE = RATE * 3 / 10, CLICK = RATE / 100 };
    static int16_t quiet[SILENCE], click[CLICK];
    RecompI3dl2 room;
    RecompAudioMix mix;
    uint64_t sends, wets, first_send, first_wet, wets_ringing, wets_after;
    Uint64 t0, t_click;
    int i;

    setenv("RECOMP_MUTE", "1", 1);
    for (i = 0; i < CLICK; i++)
        click[i] = (int16_t)(i & 1 ? 12000 : -12000);

    memset(&room, 0, sizeof room);
    room.room = 0;
    room.decay_time = 1.0f;
    room.decay_hf_ratio = 1.0f;
    room.reflections = -10000;
    room.reflections_delay = 0.01f;
    room.reverb = 0;
    room.reverb_delay = 0.01f;
    room.diffusion = 100;
    room.density = 100;
    room.hf_reference = 5000;

    memset(&mix, 0, sizeof mix);
    mix.levels[0][RECOMP_SPEAKER_FRONT_LEFT] = 1.0f;
    mix.levels[0][RECOMP_SPEAKER_FRONT_RIGHT] = 1.0f;
    mix.reverb[0] = 1.0f;
    mix.ratio = 1.0f;

    recomp_audio_output_initialize();
    t0 = SDL_GetTicksNS();
    /* The device opens on a thread of its own; the bus opens with it. */
    while (!recomp_audio_output_submit(SLOT, (const uint8_t *)quiet, 2 * CLICK, RATE, 1, 16, 0)) {
        if (SDL_GetTicksNS() - t0 > 30000000000ull) {
            printf("SKIP: no audio device within 30 s (%s)\n", SDL_GetError());
            return 0;
        }
        SDL_Delay(20);
    }
    recomp_audio_output_reset_voice(SLOT);
    recomp_audio_output_set_reverb(&room);
    recomp_audio_output_set_mix(SLOT, &mix);

    /* 1: 300 ms of silence, then the click. */
    recomp_audio_output_submit(SLOT, (const uint8_t *)quiet, sizeof quiet, RATE, 1, 16, 0);
    t_click = SDL_GetTicksNS();
    recomp_audio_output_submit(SLOT, (const uint8_t *)click, sizeof click, RATE, 1, 16, 0);
    SDL_Delay(1500);
    recomp_audio_output_reverb_stats(&sends, &wets, &first_send, &first_wet);
    if (!sends && !wets) {
        printf("SKIP: no send bus on this device (the native path plays dry)\n");
        recomp_audio_output_shutdown();
        return 0;
    }
    {
        double send_ms = first_send ? (double)(first_send - t_click) / 1e6 : -1.0;
        CHECK(sends > 0, "the click's send reached the room (%llu frames)", (unsigned long long)sends);
        CHECK(send_ms >= 250.0, "it arrived %.0f ms after it was submitted, behind the 300 ms "
              "of silence ahead of it", send_ms);
    }
    /* 2 */
    CHECK(wets > (uint64_t)sends * 10, "the room answered: %llu wet frames for %llu sent",
          (unsigned long long)wets, (unsigned long long)sends);
    CHECK(first_wet >= first_send, "the wet sound starts with the send, not before");
    recomp_audio_output_reverb_stats(NULL, &wets_ringing, NULL, NULL);
    SDL_Delay(200);
    recomp_audio_output_reverb_stats(NULL, &wets_after, NULL, NULL);
    CHECK(wets_after > wets_ringing, "still ringing 1.7 s on (a 1 s room's tail below -60 dB "
          "is still above the counters' floor)");

    /* 3 */
    recomp_audio_output_set_reverb(NULL);
    SDL_Delay(100);
    recomp_audio_output_reverb_stats(NULL, &wets, NULL, NULL);
    SDL_Delay(300);
    recomp_audio_output_reverb_stats(NULL, &wets_after, NULL, NULL);
    CHECK(wets_after == wets, "with the room cleared, nothing more comes out (%llu more frames)",
          (unsigned long long)(wets_after - wets));

    recomp_audio_output_shutdown();
    printf("%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
