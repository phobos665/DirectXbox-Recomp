/*
 * Remote play's picture (src/remote/remote_stream.c) without a game or a
 * socket: frames encoded, cut into REMOTE_VIDEO pieces as the host sends
 * them, put back together and decoded, then compared with what went in. A
 * piece is then lost, and the joiner must show nothing until a keyframe.
 */
#include "remote_stream.h"
#include "remote_pads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 640
#define H 480

typedef struct { uint8_t *data[64]; size_t len[64]; int key[64]; int n; } Packets;

static void collect(const uint8_t *data, size_t len, int key, void *user)
{
    Packets *p = (Packets *)user;
    if (p->n < 64) {
        p->data[p->n] = (uint8_t *)malloc(len);
        memcpy(p->data[p->n], data, len);
        p->len[p->n] = len;
        p->key[p->n] = key;
        p->n++;
    }
}

/* A picture that moves: a gradient shifted by t. */
static void picture(uint8_t *rgba, int t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint8_t *p = rgba + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(x + t);
            p[1] = (uint8_t)(y * 255 / H);
            p[2] = (uint8_t)(128 + ((x / 40 + y / 40 + t / 8) & 1) * 64);
            p[3] = 255;
        }
}

/* One encoded frame through the assembler, piece by piece, skipping piece
 * `drop` (-1 for none). Returns what the assembler said for the last piece. */
static int deliver(RemoteAssembler *a, uint32_t frame, const uint8_t *data, size_t len, int key,
                   int drop, const uint8_t **out, size_t *out_len, int *lost)
{
    uint16_t count = (uint16_t)((len + REMOTE_VIDEO_CHUNK - 1) / REMOTE_VIDEO_CHUNK), i;
    int r = 0, any_lost = 0;

    for (i = 0; i < count; i++) {
        size_t off = (size_t)i * REMOTE_VIDEO_CHUNK;
        size_t n = len - off < REMOTE_VIDEO_CHUNK ? len - off : REMOTE_VIDEO_CHUNK;
        int l;
        if ((int)i == drop)
            continue;
        r = remote_assembler_piece(a, frame, i, count, key, data + off, n, out, out_len, &l);
        any_lost |= l;
    }
    *lost = any_lost;
    return r;
}

int main(void)
{
    /* RECOMP_REMOTE_PLAY_ENCODER picks one, as it does for the host. */
    RemoteEncoder *enc = remote_encoder_create(getenv("RECOMP_REMOTE_PLAY_ENCODER"), W, H, 6000);
    RemoteDecoder *dec = remote_decoder_create();
    RemoteAssembler *a = remote_assembler_create();
    uint8_t *rgba = (uint8_t *)malloc((size_t)W * H * 4);
    uint8_t *inputs[10];
    Packets p;
    uint32_t frame = 0;
    int t, shown = 0;
    double worst = 0.0;

    if (!enc || !dec) {
        printf("remote_stream: no FFmpeg encoder/decoder here (%s); nothing to test\n",
               enc ? "no decoder" : "no encoder");
        return 0;
    }
    printf("remote_stream: encoder %s\n", remote_encoder_name(enc));

    /* Ten frames, the first a keyframe, all delivered whole. An encoder may
     * hand a frame back a few frames late (VideoToolbox does), so the k-th
     * picture decoded is compared with the k-th that went in. */
    for (t = 0; t < 10; t++) {
        int i;
        memset(&p, 0, sizeof p);
        picture(rgba, t * 4);
        inputs[t] = (uint8_t *)malloc((size_t)W * H * 4);
        memcpy(inputs[t], rgba, (size_t)W * H * 4);
        CHECK(remote_encoder_frame(enc, rgba, W * 4, t == 0, collect, &p) >= 0, "encode %d", t);
        for (i = 0; i < p.n; i++) {
            const uint8_t *f, *bgra;
            size_t flen;
            unsigned w, h;
            int lost;
            CHECK(t > 0 || p.key[i], "the first frame is a keyframe");
            if (deliver(a, frame++, p.data[i], p.len[i], p.key[i], -1, &f, &flen, &lost)) {
                CHECK(!lost, "nothing lost when nothing is dropped (frame %d)", t);
                if (remote_decoder_frame(dec, f, flen, &bgra, &w, &h) > 0) {
                    /* Compare with what went in: BGRA back against RGBA. */
                    double diff = 0;
                    size_t k;
                    const uint8_t *in = inputs[shown];
                    CHECK(w == W && h == H, "decoded %ux%u", w, h);
                    for (k = 0; k < (size_t)W * H; k += 97)
                        diff += abs(bgra[k * 4 + 2] - in[k * 4 + 0]) +
                                abs(bgra[k * 4 + 1] - in[k * 4 + 1]) +
                                abs(bgra[k * 4 + 0] - in[k * 4 + 2]);
                    diff /= 3.0 * ((double)W * H / 97);
                    if (diff > worst)
                        worst = diff;
                    shown++;
                }
            }
            free(p.data[i]);
        }
    }
    CHECK(shown >= 7, "%d of 10 frames shown", shown);
    for (t = 0; t < 10; t++)
        free(inputs[t]);
    CHECK(worst < 12.0, "pictures come back close to what went in (worst mean diff %.1f)", worst);

    /* A frame with a piece missing: lost, and nothing is shown until a
     * keyframe, even frames that arrive whole. */
    {
        int i, lost, r, shown_before_key = 0;
        const uint8_t *f;
        size_t flen;

        memset(&p, 0, sizeof p);
        picture(rgba, 100);
        remote_encoder_frame(enc, rgba, W * 4, 0, collect, &p);
        for (i = 0; i < p.n; i++) {
            deliver(a, frame++, p.data[i], p.len[i], p.key[i], 0, &f, &flen, &lost);
            free(p.data[i]);
        }
        /* Frames after it: the first to arrive reports the loss. */
        for (t = 0; t < 3; t++) {
            memset(&p, 0, sizeof p);
            picture(rgba, 104 + t * 4);
            remote_encoder_frame(enc, rgba, W * 4, 0, collect, &p);
            for (i = 0; i < p.n; i++) {
                r = deliver(a, frame++, p.data[i], p.len[i], p.key[i], -1, &f, &flen, &lost);
                if (t == 0 && i == 0)
                    CHECK(lost, "the next frame reports the one before as lost");
                shown_before_key += r;
                free(p.data[i]);
            }
        }
        CHECK(shown_before_key == 0, "no frame is given to the decoder before a keyframe");

        /* The keyframe asked for, within a few frames of the asking. */
        r = 0;
        for (t = 0; t < 4 && !r; t++) {
            memset(&p, 0, sizeof p);
            picture(rgba, 120 + t * 4);
            remote_encoder_frame(enc, rgba, W * 4, t == 0, collect, &p);
            for (i = 0; i < p.n; i++) {
                if (!r)
                    r = deliver(a, frame++, p.data[i], p.len[i], p.key[i], -1, &f, &flen, &lost);
                CHECK(!r || p.key[i] || i > 0, "only a keyframe ends the wait");
                free(p.data[i]);
            }
        }
        CHECK(r, "the keyframe is shown");
        {
            const uint8_t *bgra;
            unsigned w, h;
            CHECK(r && remote_decoder_frame(dec, f, flen, &bgra, &w, &h) > 0,
                  "and decodes after the loss");
        }
    }

    remote_encoder_destroy(enc);
    remote_decoder_destroy(dec);
    remote_assembler_destroy(a);
    free(rgba);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("remote_stream: all checks passed\n");
    return 0;
}
