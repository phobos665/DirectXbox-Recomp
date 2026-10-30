/*
 * remote_stream.h -- the host's picture, to remote play's joiners.
 *
 * Remote play (src/input/remote_pads.h) runs the title on the host and takes
 * controllers 2-4 from other machines; this is the picture going the other
 * way. The host hands each presented frame here, which is encoded as H.264 on
 * a thread of its own and sent in the same UDP session, a frame at a time in
 * REMOTE_VIDEO pieces. A joiner puts the pieces back together and decodes
 * them. Nothing is resent: a frame with a piece missing is dropped, the
 * joiner asks for a keyframe (REMOTE_KEYREQ), and the picture picks up again
 * from it. The host also starts every newcomer at a keyframe.
 *
 * FFmpeg is loaded at run time, as for XMV movies (src/video/xmv_decode.c),
 * so a build without it, or a machine without it, just has no picture. The
 * encoder is the first that opens, hardware first: VideoToolbox, NVENC, AMF,
 * Quick Sync, Media Foundation, then libx264 and OpenH264 in software.
 *
 *   remote_play_video   = on | off   [RECOMP_REMOTE_PLAY_VIDEO]  (on)
 *   remote_play_bitrate = <kbit/s>   [RECOMP_REMOTE_PLAY_BITRATE] (6000)
 *   remote_play_encoder = <name>     [RECOMP_REMOTE_PLAY_ENCODER] (first that opens)
 *
 * The frame is sent at the title's own size (640x480 for most): the host's
 * render is box-filtered down to it when the host renders larger.
 */
#ifndef XBOXRECOMP_REMOTE_STREAM_H
#define XBOXRECOMP_REMOTE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the host --------------------------------------------------------- */

/* Whether a frame handed over now would be sent: a remote player is in, the
 * picture is on and an encoder opened. Cheap; ask before reading back. */
int  remote_stream_wanted(void);
/* A presented frame, R8G8B8A8, `pitch` bytes a row, at the host's render size
 * (src_w x src_h), to be sent at the title's size (out_w x out_h). Copied, so
 * the caller's buffer is free on return; the encoding happens elsewhere, and
 * a frame that arrives while the last is still being encoded replaces it. */
void remote_stream_frame(const uint8_t *rgba, unsigned src_w, unsigned src_h, unsigned pitch,
                         unsigned out_w, unsigned out_h);

/* ---- the joiner ------------------------------------------------------- */

/* Puts a frame's REMOTE_VIDEO pieces back together. */
typedef struct RemoteAssembler RemoteAssembler;
RemoteAssembler *remote_assembler_create(void);
void remote_assembler_destroy(RemoteAssembler *a);
/* One piece. Returns 1 when it completes a frame that can be decoded --
 * *frame, *len and *key then describe it, valid until the next call -- and
 * sets *lost when a frame before it never completed (ask for a keyframe;
 * frames are not decodable again until one arrives). */
int  remote_assembler_piece(RemoteAssembler *a, uint32_t frame, uint16_t index, uint16_t count,
                            int key, const uint8_t *data, size_t len,
                            const uint8_t **out, size_t *out_len, int *lost);

typedef struct RemoteDecoder RemoteDecoder;
/* NULL when there is no FFmpeg or no H.264 decoder in it. */
RemoteDecoder *remote_decoder_create(void);
void remote_decoder_destroy(RemoteDecoder *d);
/* A whole frame. 1 when a picture came out: *bgra (B8G8R8A8, w*4 a row) is
 * valid until the next call. */
int  remote_decoder_frame(RemoteDecoder *d, const uint8_t *data, size_t len,
                          const uint8_t **bgra, unsigned *w, unsigned *h);

/* ---- for the test ----------------------------------------------------- */

/* An encoder on its own, without the session: name NULL for the first that
 * opens. Each packet the encoder produces goes to `out`. */
typedef struct RemoteEncoder RemoteEncoder;
RemoteEncoder *remote_encoder_create(const char *name, unsigned w, unsigned h, int kbps);
const char    *remote_encoder_name(const RemoteEncoder *e);
int  remote_encoder_frame(RemoteEncoder *e, const uint8_t *rgba, unsigned pitch, int key,
                          void (*out)(const uint8_t *data, size_t len, int key, void *user),
                          void *user);
void remote_encoder_destroy(RemoteEncoder *e);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_REMOTE_STREAM_H */
