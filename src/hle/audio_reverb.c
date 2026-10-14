/* An I3DL2 room reverb: see audio_reverb.h. */
#include "audio_reverb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LINES      8
#define ALLPASSES  4
#define TAPS       6
#define MAX_PRE_S  0.45          /* reflections_delay 0.3 + reverb_delay 0.1, and slack */

/* Line lengths in ms at full density: mutually prime-ish, so the echoes
 * do not pile up on one period. Scaled down to half at density 0. */
static const float k_line_ms[LINES] = { 29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.9f, 67.7f };
static const float k_ap_ms[ALLPASSES] = { 4.77f, 3.59f, 2.73f, 1.73f };
/* Early reflections: times as multiples of reflections_delay, and sides. */
static const float k_tap_at[TAPS]  = { 1.00f, 1.19f, 1.43f, 1.71f, 2.03f, 2.37f };
static const float k_tap_side[TAPS] = { 1.0f, -1.0f, 0.8f, -0.8f, 0.6f, -0.6f };

typedef struct { float *buf; uint32_t len, pos; } Delay;

struct RecompReverb {
    uint32_t rate;
    int      active;
    /* Parameters, as gains and lengths. */
    float    wet_early, wet_late;             /* room x reflections, room x reverb */
    float    hf_coef, hf_gain;                /* the room's HF cut: one pole at hf_reference */
    uint32_t tap_delay[TAPS], late_delay;
    float    fb[LINES], damp[LINES];          /* per line: DC gain, one-pole coefficient */
    float    ap_coef;
    uint32_t line_len[LINES], ap_len[ALLPASSES];
    /* State. */
    float    hf_state;
    Delay    pre, line[LINES], ap[ALLPASSES];
    float    damp_state[LINES];
};

static int delay_init(Delay *d, uint32_t len)
{
    d->buf = (float *)calloc(len ? len : 1u, sizeof(float));
    d->len = len ? len : 1u;
    d->pos = 0;
    return d->buf != NULL;
}

static float delay_read(const Delay *d, uint32_t back)
{
    uint32_t i = d->pos + d->len - (back % d->len);
    return d->buf[i % d->len];
}

static void delay_write(Delay *d, float v)
{
    d->buf[d->pos] = v;
    d->pos = (d->pos + 1u) % d->len;
}

static float mb_to_gain(float mb) { return mb <= -10000.0f ? 0.0f : powf(10.0f, mb / 2000.0f); }

RecompReverb *recomp_reverb_create(uint32_t rate)
{
    RecompReverb *r = (RecompReverb *)calloc(1, sizeof *r);
    uint32_t i;
    if (!r)
        return NULL;
    r->rate = rate ? rate : 48000u;
    if (!delay_init(&r->pre, (uint32_t)(MAX_PRE_S * r->rate) + 1u))
        goto fail;
    for (i = 0; i < LINES; i++)
        if (!delay_init(&r->line[i], (uint32_t)(k_line_ms[i] * 0.001f * r->rate) + 1u))
            goto fail;
    for (i = 0; i < ALLPASSES; i++)
        if (!delay_init(&r->ap[i], (uint32_t)(k_ap_ms[i] * 0.001f * r->rate) + 1u))
            goto fail;
    return r;
fail:
    recomp_reverb_destroy(r);
    return NULL;
}

void recomp_reverb_destroy(RecompReverb *r)
{
    uint32_t i;
    if (!r)
        return;
    free(r->pre.buf);
    for (i = 0; i < LINES; i++) free(r->line[i].buf);
    for (i = 0; i < ALLPASSES; i++) free(r->ap[i].buf);
    free(r);
}

int recomp_reverb_active(const RecompReverb *r) { return r && r->active; }

void recomp_reverb_set_room(RecompReverb *r, const RecompI3dl2 *room)
{
    float fs, decay, decay_hf, density, room_gain, refl_delay, rev_delay, w;
    uint32_t i;

    if (!r)
        return;
    if (!room || room->room <= -10000) {
        r->active = 0;
        return;
    }
    fs = (float)r->rate;
    room_gain = mb_to_gain((float)room->room);
    r->wet_early = room_gain * mb_to_gain((float)room->reflections);
    r->wet_late = room_gain * mb_to_gain((float)room->reverb);

    /* The room's HF level at hf_reference: a one-pole lowpass at that
     * frequency, with the rest scaled by room_hf. */
    w = 2.0f * 3.14159265f * fminf(fmaxf(room->hf_reference, 20.0f), 20000.0f) / fs;
    r->hf_coef = expf(-w);
    r->hf_gain = mb_to_gain((float)room->room_hf);

    refl_delay = fminf(fmaxf(room->reflections_delay, 0.0f), 0.3f);
    rev_delay = fminf(fmaxf(room->reverb_delay, 0.0f), 0.1f);
    for (i = 0; i < TAPS; i++) {
        float t = refl_delay * k_tap_at[i];
        if (t > (float)MAX_PRE_S - 0.01f) t = (float)MAX_PRE_S - 0.01f;
        r->tap_delay[i] = (uint32_t)(t * fs) + 1u;
    }
    r->late_delay = (uint32_t)((refl_delay + rev_delay) * fs) + 1u;

    /* Late reverb: each line's gain makes a 60 dB decay in decay_time, and
     * its one-pole damping makes the top end decay in decay_time x
     * decay_hf_ratio (Jot's absorptive delay network). */
    decay = fminf(fmaxf(room->decay_time, 0.1f), 20.0f);
    decay_hf = decay * fminf(fmaxf(room->decay_hf_ratio, 0.1f), 2.0f);
    density = fminf(fmaxf(room->density, 0.0f), 100.0f) / 100.0f;
    for (i = 0; i < LINES; i++) {
        float len_s = k_line_ms[i] * 0.001f * (0.5f + 0.5f * density);
        float g = powf(10.0f, -3.0f * len_s / decay);
        float g_hf = powf(10.0f, -3.0f * len_s / decay_hf);
        r->line_len[i] = (uint32_t)(len_s * fs);
        if (r->line_len[i] < 1u) r->line_len[i] = 1u;
        if (r->line_len[i] > r->line[i].len) r->line_len[i] = r->line[i].len;
        r->fb[i] = g;
        r->damp[i] = (g - g_hf) / (g + g_hf);          /* 0 when the two decays agree */
        if (r->damp[i] < 0.0f) r->damp[i] = 0.0f;      /* no brighter-than-DC tails */
    }
    r->ap_coef = 0.3f + 0.4f * fminf(fmaxf(room->diffusion, 0.0f), 100.0f) / 100.0f;
    for (i = 0; i < ALLPASSES; i++)
        r->ap_len[i] = r->ap[i].len;
    r->active = 1;
}

void recomp_reverb_process(RecompReverb *r, const float *in, uint32_t in_stride,
                           float *out_l, float *out_r, uint32_t out_stride,
                           uint32_t count)
{
    uint32_t n, i;
    if (!r || !r->active)
        return;
    for (n = 0; n < count; n++) {
        float x = in[(size_t)n * in_stride];
        float lp, early_l = 0.0f, early_r = 0.0f, late_in, late_l = 0.0f, late_r = 0.0f;
        float out[LINES], sum = 0.0f;

        /* The room's HF cut. */
        r->hf_state = (1.0f - r->hf_coef) * x + r->hf_coef * r->hf_state;
        lp = r->hf_state;
        x = lp + r->hf_gain * (x - lp);
        delay_write(&r->pre, x);

        /* Early reflections. */
        for (i = 0; i < TAPS; i++) {
            float t = delay_read(&r->pre, r->tap_delay[i]) * (1.0f / TAPS);
            if (k_tap_side[i] > 0) { early_l += t * k_tap_side[i]; early_r += t * (1.0f - k_tap_side[i]); }
            else                   { early_r += t * -k_tap_side[i]; early_l += t * (1.0f + k_tap_side[i]); }
        }

        /* Into the tail, through the diffusing allpasses. */
        late_in = delay_read(&r->pre, r->late_delay);
        for (i = 0; i < ALLPASSES; i++) {
            float d = delay_read(&r->ap[i], r->ap_len[i]);
            float v = late_in + r->ap_coef * d;
            delay_write(&r->ap[i], v);
            late_in = d - r->ap_coef * v;
        }

        /* The delay network: read each line, damp it, mix through an
         * 8x8 Householder matrix (out - 2/N * sum), feed back with the input. */
        for (i = 0; i < LINES; i++) {
            float v = delay_read(&r->line[i], r->line_len[i]);
            r->damp_state[i] = (1.0f - r->damp[i]) * v + r->damp[i] * r->damp_state[i];
            out[i] = r->damp_state[i] * r->fb[i];
            sum += out[i];
        }
        for (i = 0; i < LINES; i++) {
            float fb = out[i] - (2.0f / LINES) * sum;
            delay_write(&r->line[i], late_in + fb);
            if (i & 1u) late_r += out[i]; else late_l += out[i];
        }

        out_l[(size_t)n * out_stride] += r->wet_early * early_l + r->wet_late * late_l * 0.5f;
        out_r[(size_t)n * out_stride] += r->wet_early * early_r + r->wet_late * late_r * 0.5f;
    }
}
