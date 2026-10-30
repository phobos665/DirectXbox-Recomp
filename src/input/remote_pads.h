/*
 * remote_pads.h -- controllers 2 to 4 played from another machine.
 *
 * Remote play: the title runs once, on the host, and its player is
 * controller 1. Friends join over the internet as controllers 2, 3 and 4,
 * each with a pad (or keyboard) on their own machine. Split-screen and
 * shared-screen titles need nothing else -- there is only one copy of the
 * game, so nothing has to stay in step. The picture and the sound go back
 * the other way (not yet: the joiner sees the game some other way for now).
 *
 *   remote_play      = off | host        [RECOMP_REMOTE_PLAY]
 *   remote_play_port = 27620             [RECOMP_REMOTE_PLAY_PORT]
 *   remote_play_code = <text>            [RECOMP_REMOTE_PLAY_CODE]
 *
 * The host listens on that UDP port (and asks the router to forward it, as
 * System Link does). A joiner (tools: xboxrecomp-remote) says hello with the
 * code, is given the lowest of ports 2-4 that has no pad of the host's own
 * on it, and from then on sends its pad's whole state about 125 times a
 * second; a lost packet costs nothing, because the next one says it all.
 * Its port is unplugged when it says goodbye or goes quiet for three
 * seconds, so the title sees a controller pulled out and put back as the
 * console would. Rumble on its port goes back to it.
 *
 * The wire format is here so the host, the joiner and the test share it.
 * Little-endian, every message one datagram:
 *
 *   u32 magic 'XRRP', u8 version, u8 type, then by type --
 *   HELLO    char code[16], char name[24]
 *   WELCOME  u8 port (1-based)
 *   REFUSED  u8 reason (REMOTE_REFUSED_*)
 *   PAD      u32 sequence, u16 buttons, u8 analog[8], i16 lx, ly, rx, ry
 *   RUMBLE   u16 low, u16 high
 *   BYE
 *   PING     u32 stamp            (answered with PONG, the same stamp)
 */
#ifndef XBOXRECOMP_REMOTE_PADS_H
#define XBOXRECOMP_REMOTE_PADS_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_MAGIC        0x50525258u     /* "XRRP" */
#define REMOTE_VERSION      1
#define REMOTE_DEFAULT_PORT 27620
#define REMOTE_TIMEOUT_MS   3000

enum {
    REMOTE_HELLO = 1, REMOTE_WELCOME, REMOTE_REFUSED, REMOTE_PAD,
    REMOTE_RUMBLE, REMOTE_BYE, REMOTE_PING, REMOTE_PONG
};
enum {
    REMOTE_REFUSED_FULL = 1,     /* ports 2-4 are all taken          */
    REMOTE_REFUSED_CODE,         /* the session code does not match  */
    REMOTE_REFUSED_VERSION       /* the joiner speaks another version */
};

/* ---- the wire --------------------------------------------------------- */

typedef struct RemoteMsg {
    int      type;
    char     code[17], name[25];        /* HELLO, NUL-terminated here */
    int      port;                      /* WELCOME, 1-based           */
    int      reason;                    /* REFUSED                    */
    uint32_t seq;                       /* PAD                        */
    XBOX_GAMEPAD pad;                   /* PAD                        */
    uint16_t low, high;                 /* RUMBLE                     */
    uint32_t stamp;                     /* PING / PONG                */
} RemoteMsg;

/* Bytes written to buf (at least 64 long), 0 for an unknown type. */
size_t remote_encode(const RemoteMsg *m, uint8_t *buf, size_t cap);
/* 1 and *m filled for a well-formed message of this version or a HELLO of
 * any version (so a joiner can be told it is out of date), else 0. */
int    remote_decode(const uint8_t *buf, size_t len, RemoteMsg *m, int *version);

/* ---- the host --------------------------------------------------------- */

/* Starts hosting if remote_play = host, once. Called by the bindings at
 * their first use, so only a running title ever hosts. */
void     recomp_remote_init(void);
/* For a program that reads the bindings but must never host: the joiner.
 * Call before anything touches the bindings. */
void     recomp_remote_never_host(void);
/* Host on this port with this code now, whatever the settings say; for the
 * test. 0 on success. port 0 picks a free one, which *bound gets. */
int      recomp_remote_host_start(int port, const char *code, int *bound);
void     recomp_remote_host_stop(void);

/* Bit n-1 set: port n is held by a remote player. */
unsigned recomp_remote_mask(void);
/* Port n's (0-based) remote state; 0 when no remote player holds it. */
int      recomp_remote_sample(unsigned port, XBOX_GAMEPAD *out);
/* Rumble for port n (0-based): sent to its player, if one holds it. */
void     recomp_remote_rumble(unsigned port, unsigned short low, unsigned short high);
/* The player's name on port n (0-based), "" if none. */
const char *recomp_remote_name(unsigned port);

/* The ports a joiner may be given: the bindings say which of 2-4 have a
 * device of the host's own present (bit n-1 for port n). Set by the
 * bindings; remote players only ever take the others. */
void     recomp_remote_set_local_mask_fn(unsigned (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_REMOTE_PADS_H */
