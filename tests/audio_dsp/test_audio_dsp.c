/*
 * The audio path's plain-C stages, with no device and no game:
 *
 *   - the fold-down from Xbox speakers to a host's (src/hle/audio_speakers.c)
 *     against the XAudio2 backend's rules;
 *   - the I3DL2 room reverb (src/hle/audio_reverb.c): an empty room is
 *     silent, a room's tail dies away in its decay_time, a low
 *     decay_hf_ratio kills the top end sooner, and a long, bright room stays
 *     bounded.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_speakers.h"
#include "audio_reverb.h"

static int g_failed;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failed = 1; } \
        else         { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); }              \
    } while (0)

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_fold(void)
{
    float f[RECOMP_SPEAKERS][RECOMP_FOLD_MAX_CHANNELS];
    const float m3 = 0.70710678f;

    recomp_audio_fold_matrix(2, f);
    CHECK(near(f[RECOMP_SPEAKER_FRONT_LEFT][0], 1) && near(f[RECOMP_SPEAKER_FRONT_LEFT][1], 0),
          "stereo: front left to left at full level");
    CHECK(near(f[RECOMP_SPEAKER_CENTER][0], m3) && near(f[RECOMP_SPEAKER_CENTER][1], m3),
          "stereo: centre to both fronts at -3 dB");
    CHECK(near(f[RECOMP_SPEAKER_LFE][0], 0) && near(f[RECOMP_SPEAKER_LFE][1], 0),
          "stereo: LFE dropped");
    CHECK(near(f[RECOMP_SPEAKER_BACK_RIGHT][1], m3) && near(f[RECOMP_SPEAKER_BACK_RIGHT][0], 0),
          "stereo: back right to right at -3 dB");

    recomp_audio_fold_matrix(1, f);
    CHECK(near(f[RECOMP_SPEAKER_FRONT_RIGHT][0], m3) && near(f[RECOMP_SPEAKER_LFE][0], 0),
          "mono: every speaker but the LFE at -3 dB");

    recomp_audio_fold_matrix(6, f);
    {
        int s, k, identity = 1;
        for (s = 0; s < RECOMP_SPEAKERS; s++)
            for (k = 0; k < 6; k++)
                identity &= near(f[s][k], s == k ? 1.0f : 0.0f);
        CHECK(identity, "5.1: each speaker to itself");
    }
}

static RecompI3dl2 room(float decay, float hf_ratio)
{
    RecompI3dl2 r;
    memset(&r, 0, sizeof r);
    r.room = 0;
    r.room_hf = 0;
    r.decay_time = decay;
    r.decay_hf_ratio = hf_ratio;
    r.reflections = -10000;          /* the tail only */
    r.reflections_delay = 0.01f;
    r.reverb = 0;
    r.reverb_delay = 0.01f;
    r.diffusion = 100;
    r.density = 100;
    r.hf_reference = 5000;
    return r;
}

/* Impulse response, left channel, `seconds` long at 48 kHz. */
static float *impulse(const RecompI3dl2 *rm, float seconds, uint32_t *len)
{
    const uint32_t rate = 48000, n = (uint32_t)(seconds * rate);
    RecompReverb *r = recomp_reverb_create(rate);
    float *in = calloc(n, sizeof(float)), *l = calloc(n, sizeof(float)), *rr = calloc(n, sizeof(float));
    in[0] = 1.0f;
    recomp_reverb_set_room(r, rm);
    recomp_reverb_process(r, in, 1, l, rr, 1, n);
    recomp_reverb_destroy(r);
    free(in);
    free(rr);
    *len = n;
    return l;
}

/* T60 by Schroeder's backward integration, from the slope between -5 and
 * -25 dB. `hf` takes the first difference first, which weights the top. */
static float t60(const float *ir, uint32_t n, int hf)
{
    double *e = calloc(n + 1, sizeof(double)), total;
    uint32_t i, a = 0, b = 0;
    for (i = n; i-- > 0;) {
        double x = hf ? ir[i] - (i ? ir[i - 1] : 0.0f) : ir[i];
        e[i] = e[i + 1] + x * x;
    }
    total = e[0];
    for (i = 0; i < n; i++) {
        double db = 10.0 * log10(e[i] / total + 1e-30);
        if (!a && db <= -5.0) a = i;
        if (!b && db <= -25.0) { b = i; break; }
    }
    free(e);
    if (!a || !b || b <= a)
        return 0.0f;
    return (float)((b - a) / 48000.0 * 3.0);   /* 20 dB of slope, times three */
}

static void test_reverb(void)
{
    uint32_t n, i;
    float *ir, peak = 0.0f;
    RecompI3dl2 empty = room(1.0f, 1.0f);

    empty.room = -10000;
    ir = impulse(&empty, 0.5f, &n);
    for (i = 0; i < n; i++) peak = fmaxf(peak, fabsf(ir[i]));
    CHECK(peak == 0.0f, "an empty room (-10000) is silent");
    free(ir);

    {
        RecompI3dl2 r1 = room(1.0f, 1.0f), r3 = room(3.0f, 1.0f);
        float a, b;
        ir = impulse(&r1, 3.0f, &n); a = t60(ir, n, 0); free(ir);
        ir = impulse(&r3, 8.0f, &n); b = t60(ir, n, 0); free(ir);
        CHECK(a > 0.75f && a < 1.25f, "decay_time 1.0 s: T60 %.2f s", (double)a);
        CHECK(b > 2.25f && b < 3.75f, "decay_time 3.0 s: T60 %.2f s", (double)b);
    }
    {
        RecompI3dl2 flat = room(2.0f, 1.0f), dull = room(2.0f, 0.3f);
        float a, b;
        ir = impulse(&flat, 5.0f, &n); a = t60(ir, n, 1); free(ir);
        ir = impulse(&dull, 5.0f, &n); b = t60(ir, n, 1); free(ir);
        CHECK(b < 0.6f * a, "decay_hf_ratio 0.3: the top dies sooner (%.2f s against %.2f s)",
              (double)b, (double)a);
    }
    {
        RecompI3dl2 big = room(20.0f, 2.0f);
        RecompReverb *r = recomp_reverb_create(48000);
        const uint32_t m = 48000 * 10;
        float *in = calloc(m, sizeof(float)), *l = calloc(m, sizeof(float)), *rr = calloc(m, sizeof(float));
        unsigned s = 1;
        for (i = 0; i < m / 2; i++) {
            s = s * 1103515245u + 12345u;
            in[i] = ((float)((s >> 9) & 0xFFFF) / 32768.0f - 1.0f) * 0.5f;
        }
        recomp_reverb_set_room(r, &big);
        recomp_reverb_process(r, in, 1, l, rr, 1, m);
        peak = 0.0f;
        for (i = 0; i < m; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(rr[i])));
        CHECK(isfinite(peak) && peak < 20.0f,
              "a 20 s, bright room under 5 s of noise stays bounded (peak %.2f)", (double)peak);
        recomp_reverb_destroy(r);
        free(in); free(l); free(rr);
    }
}

int main(void)
{
    test_fold();
    test_reverb();
    printf(g_failed ? "audio_dsp: FAILED\n" : "audio_dsp: all passed\n");
    return g_failed;
}
