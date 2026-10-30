/*
 * remote_pads.c -- controllers 2 to 4 played from another machine
 * (remote_pads.h). The wire format, and the host: one UDP socket and a thread
 * that hands out ports and keeps each remote player's latest pad.
 */
#include "remote_pads.h"
#include "recomp_config.h"
#include "net_upnp.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define SOCK_BAD INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <time.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_BAD (-1)
#  define sock_close close
#endif

/* ---- the wire --------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

size_t remote_encode(const RemoteMsg *m, uint8_t *buf, size_t cap)
{
    size_t n = 6;
    int i;

    if (cap < 64 || (m->type == REMOTE_VIDEO && cap < 17u + m->length))
        return 0;
    memset(buf, 0, 64);
    put32(buf, REMOTE_MAGIC);
    buf[4] = REMOTE_VERSION;
    buf[5] = (uint8_t)m->type;
    switch (m->type) {
    case REMOTE_HELLO:
        memcpy(buf + 6, m->code, strnlen(m->code, 16));
        memcpy(buf + 22, m->name, strnlen(m->name, 24));
        n = 46;
        break;
    case REMOTE_WELCOME: buf[6] = (uint8_t)m->port;   n = 7; break;
    case REMOTE_REFUSED: buf[6] = (uint8_t)m->reason; n = 7; break;
    case REMOTE_PAD:
        put32(buf + 6, m->seq);
        put16(buf + 10, m->pad.wButtons);
        for (i = 0; i < 8; i++)
            buf[12 + i] = m->pad.bAnalogButtons[i];
        put16(buf + 20, (uint16_t)m->pad.sThumbLX);
        put16(buf + 22, (uint16_t)m->pad.sThumbLY);
        put16(buf + 24, (uint16_t)m->pad.sThumbRX);
        put16(buf + 26, (uint16_t)m->pad.sThumbRY);
        n = 28;
        break;
    case REMOTE_RUMBLE: put16(buf + 6, m->low); put16(buf + 8, m->high); n = 10; break;
    case REMOTE_BYE: break;
    case REMOTE_PING: case REMOTE_PONG: put32(buf + 6, m->stamp); n = 10; break;
    case REMOTE_VIDEO:
        put32(buf + 6, m->frame);
        put16(buf + 10, m->index);
        put16(buf + 12, m->count);
        buf[14] = m->flags;
        put16(buf + 15, m->length);
        memcpy(buf + 17, m->data, m->length);
        n = 17u + m->length;
        break;
    case REMOTE_KEYREQ: break;
    default: return 0;
    }
    return n;
}

int remote_decode(const uint8_t *buf, size_t len, RemoteMsg *m, int *version)
{
    static const size_t need[] = { 0, 46, 7, 7, 28, 10, 6, 10, 10, 17, 6 };
    int i;

    memset(m, 0, sizeof *m);
    if (len < 6 || get32(buf) != REMOTE_MAGIC)
        return 0;
    if (version)
        *version = buf[4];
    m->type = buf[5];
    if (m->type < REMOTE_HELLO || m->type > REMOTE_KEYREQ || len < need[m->type])
        return 0;
    if (buf[4] != REMOTE_VERSION && m->type != REMOTE_HELLO)
        return 0;
    switch (m->type) {
    case REMOTE_HELLO:
        memcpy(m->code, buf + 6, 16);
        memcpy(m->name, buf + 22, 24);
        break;
    case REMOTE_WELCOME: m->port = buf[6];   break;
    case REMOTE_REFUSED: m->reason = buf[6]; break;
    case REMOTE_PAD:
        m->seq = get32(buf + 6);
        m->pad.wButtons = (WORD)get16(buf + 10);
        for (i = 0; i < 8; i++)
            m->pad.bAnalogButtons[i] = buf[12 + i];
        m->pad.sThumbLX = (SHORT)get16(buf + 20);
        m->pad.sThumbLY = (SHORT)get16(buf + 22);
        m->pad.sThumbRX = (SHORT)get16(buf + 24);
        m->pad.sThumbRY = (SHORT)get16(buf + 26);
        break;
    case REMOTE_RUMBLE: m->low = (uint16_t)get16(buf + 6); m->high = (uint16_t)get16(buf + 8); break;
    case REMOTE_PING: case REMOTE_PONG: m->stamp = get32(buf + 6); break;
    case REMOTE_VIDEO:
        m->frame = get32(buf + 6);
        m->index = (uint16_t)get16(buf + 10);
        m->count = (uint16_t)get16(buf + 12);
        m->flags = buf[14];
        m->length = (uint16_t)get16(buf + 15);
        if (len < 17u + m->length || m->length > REMOTE_VIDEO_CHUNK || !m->count ||
            m->index >= m->count)
            return 0;
        m->data = buf + 17;
        break;
    default: break;
    }
    return 1;
}

/* ---- the host --------------------------------------------------------- */

#define PORTS 4

typedef struct Player {
    int                in_use;
    struct sockaddr_in from;
    char               name[25];
    uint32_t           seq;
    XBOX_GAMEPAD       pad;
    uint64_t           last_ms;
} Player;

static struct {
    SDL_Mutex     *lock;
    SDL_Thread    *thread;
    sock_t         sock;
    volatile int   running;
    char           code[17];
    Player         players[PORTS];          /* [0] is never used: the host's */
    unsigned     (*local_mask)(void);
    int            never_host, started;
    volatile int   key_wanted;
} H = { .sock = SOCK_BAD };

void recomp_remote_set_local_mask_fn(unsigned (*fn)(void)) { H.local_mask = fn; }
void recomp_remote_never_host(void) { H.never_host = 1; }

/* The host's clock for its players: monotonic, and its own. SDL_GetTicks
 * starts again when the title brings SDL up, and a player who joined before
 * that was dropped as gone quiet the moment it did. */
static uint64_t host_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_to(const struct sockaddr_in *to, const RemoteMsg *m)
{
    uint8_t buf[64];
    size_t n = remote_encode(m, buf, sizeof buf);
    if (n)
        sendto(H.sock, (const char *)buf, (int)n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void say(const char *fmt, const char *name, int port)
{
    fprintf(stderr, fmt, name, port);
    fflush(stderr);
}

/* Under the lock. The player this datagram comes from, or -1. */
static int find_player(const struct sockaddr_in *from)
{
    int i;
    for (i = 1; i < PORTS; i++)
        if (H.players[i].in_use && same_peer(&H.players[i].from, from))
            return i;
    return -1;
}

static void on_hello(const struct sockaddr_in *from, const RemoteMsg *m, int version)
{
    RemoteMsg r;
    unsigned local = H.local_mask ? H.local_mask() : 0;
    int i;

    memset(&r, 0, sizeof r);
    if (version != REMOTE_VERSION) {
        r.type = REMOTE_REFUSED;
        r.reason = REMOTE_REFUSED_VERSION;
        send_to(from, &r);
        return;
    }
    if (H.code[0] && strcmp(H.code, m->code) != 0) {
        r.type = REMOTE_REFUSED;
        r.reason = REMOTE_REFUSED_CODE;
        send_to(from, &r);
        fprintf(stderr, "[REMOTE] a player asked to join with the wrong code\n");
        return;
    }
    SDL_LockMutex(H.lock);
    i = find_player(from);                   /* a hello again: same port */
    if (i < 0)
        for (i = 1; i < PORTS; i++)
            if (!H.players[i].in_use && !(local & (1u << i)))
                break;
    if (i >= PORTS) {
        SDL_UnlockMutex(H.lock);
        r.type = REMOTE_REFUSED;
        r.reason = REMOTE_REFUSED_FULL;
        send_to(from, &r);
        fprintf(stderr, "[REMOTE] a player asked to join; ports 2-4 are all taken\n");
        return;
    }
    if (!H.players[i].in_use) {
        memset(&H.players[i], 0, sizeof H.players[i]);
        H.players[i].in_use = 1;
        H.players[i].from = *from;
        snprintf(H.players[i].name, sizeof H.players[i].name, "%s",
                 m->name[0] ? m->name : "a friend");
        say("[REMOTE] %s joined as controller %d\n", H.players[i].name, i + 1);
        H.key_wanted = 1;                    /* its picture starts at a keyframe */
    }
    H.players[i].last_ms = host_ms();
    SDL_UnlockMutex(H.lock);
    r.type = REMOTE_WELCOME;
    r.port = i + 1;
    send_to(from, &r);
}

static void drop(int i, const char *why)
{
    char name[25];
    snprintf(name, sizeof name, "%s", H.players[i].name);
    memset(&H.players[i], 0, sizeof H.players[i]);
    fprintf(stderr, "[REMOTE] %s (controller %d) %s\n", name, i + 1, why);
    fflush(stderr);
}

static int SDLCALL host_thread(void *arg)
{
    (void)arg;
    while (H.running) {
        uint8_t buf[128];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        RemoteMsg m;
        int n, version, i;
        uint64_t now;

        n = (int)recvfrom(H.sock, (char *)buf, (int)sizeof buf, 0,
                          (struct sockaddr *)&from, &fl);
        now = host_ms();
        if (n > 0 && remote_decode(buf, (size_t)n, &m, &version)) {
            if (m.type == REMOTE_HELLO) {
                on_hello(&from, &m, version);
            } else if (m.type == REMOTE_PING) {
                m.type = REMOTE_PONG;
                send_to(&from, &m);
            } else {
                SDL_LockMutex(H.lock);
                i = find_player(&from);
                if (i >= 0) {
                    H.players[i].last_ms = now;
                    /* Newest wins; a datagram that arrives late says less. */
                    if (m.type == REMOTE_PAD && (int32_t)(m.seq - H.players[i].seq) > 0) {
                        H.players[i].seq = m.seq;
                        H.players[i].pad = m.pad;
                    } else if (m.type == REMOTE_BYE) {
                        drop(i, "left");
                    } else if (m.type == REMOTE_KEYREQ) {
                        H.key_wanted = 1;
                    }
                }
                SDL_UnlockMutex(H.lock);
                if (i < 0 && m.type == REMOTE_PAD) {
                    /* A joiner this host does not know (dropped, or the
                     * host started again): it should say hello. */
                    RemoteMsg r;
                    memset(&r, 0, sizeof r);
                    r.type = REMOTE_REFUSED;
                    r.reason = REMOTE_REFUSED_UNKNOWN;
                    send_to(&from, &r);
                }
            }
        }
        /* The clock read again: a hello just handled stamped its player
         * after `now`, and against the old reading it had been quiet for
         * minus 200 ms, wrapped -- dropped the moment it joined. */
        now = host_ms();
        SDL_LockMutex(H.lock);
        for (i = 1; i < PORTS; i++)
            if (H.players[i].in_use && now > H.players[i].last_ms &&
                now - H.players[i].last_ms > REMOTE_TIMEOUT_MS) {
                char why[64];
                snprintf(why, sizeof why, "went quiet for %llu ms and was unplugged",
                         (unsigned long long)(now - H.players[i].last_ms));
                drop(i, why);
            }
        SDL_UnlockMutex(H.lock);
    }
    return 0;
}

int recomp_remote_host_start(int port, const char *code, int *bound)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
#ifdef _WIN32
    WSADATA wsa;
    DWORD tv = 100;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
#else
    struct timeval tv = { 0, 100000 };
#endif

    if (H.running)
        return 0;
    if (!H.lock && !(H.lock = SDL_CreateMutex()))
        return -1;
    H.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (H.sock == SOCK_BAD)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(H.sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "[REMOTE] cannot listen on UDP port %d: it is in use\n", port);
        sock_close(H.sock);
        H.sock = SOCK_BAD;
        return -1;
    }
    /* A receive that gives up every 100 ms, so quiet players are noticed
     * and stopping does not wait for a packet. */
    setsockopt(H.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    getsockname(H.sock, (struct sockaddr *)&a, &al);
    if (bound)
        *bound = ntohs(a.sin_port);
    snprintf(H.code, sizeof H.code, "%s", code ? code : "");
    memset(H.players, 0, sizeof H.players);
    H.running = 1;
    H.thread = SDL_CreateThread(host_thread, "remote play", NULL);
    if (!H.thread) {
        H.running = 0;
        sock_close(H.sock);
        H.sock = SOCK_BAD;
        return -1;
    }
    return 0;
}

void recomp_remote_host_stop(void)
{
    if (!H.running)
        return;
    H.running = 0;
    SDL_WaitThread(H.thread, NULL);
    H.thread = NULL;
    sock_close(H.sock);
    H.sock = SOCK_BAD;
    memset(H.players, 0, sizeof H.players);
}

void recomp_remote_init(void)
{
    const char *mode, *code;
    int port, bound = 0;

    if (H.started || H.never_host)
        return;
    H.started = 1;
    mode = recomp_config_lookup("RECOMP_REMOTE_PLAY", "remote_play");
    if (!mode || strcmp(mode, "host") != 0)
        return;
    port = recomp_config_int("RECOMP_REMOTE_PLAY_PORT", "remote_play_port", REMOTE_DEFAULT_PORT);
    code = recomp_config_lookup("RECOMP_REMOTE_PLAY_CODE", "remote_play_code");
    if (recomp_remote_host_start(port, code, &bound) != 0)
        return;
    {
        char lan[64];
        if (!net_local_address(lan, sizeof lan))
            snprintf(lan, sizeof lan, "this machine");
        fprintf(stderr, "[REMOTE] hosting remote play on UDP port %d%s: friends join "
                "as controllers 2-4 at %s:%d (from the internet, the router's address)\n",
                bound, code && *code ? " with a code" : "", lan, bound);
        fflush(stderr);
    }
    if (recomp_config_int("RECOMP_SYSLINK_UPNP", "syslink_upnp", 1))
        net_upnp_open_async(bound);
}

unsigned recomp_remote_mask(void)
{
    unsigned mask = 0;
    int i;

    if (!H.running)
        return 0;
    SDL_LockMutex(H.lock);
    for (i = 1; i < PORTS; i++)
        if (H.players[i].in_use)
            mask |= 1u << i;
    SDL_UnlockMutex(H.lock);
    return mask;
}

int recomp_remote_sample(unsigned port, XBOX_GAMEPAD *out)
{
    int held = 0;

    if (!H.running || port == 0 || port >= PORTS)
        return 0;
    SDL_LockMutex(H.lock);
    if (H.players[port].in_use) {
        *out = H.players[port].pad;
        held = 1;
    }
    SDL_UnlockMutex(H.lock);
    return held;
}

int recomp_remote_keyframe_wanted(void)
{
    int k = H.key_wanted;
    H.key_wanted = 0;
    return k;
}

void recomp_remote_send_video(uint32_t frame, const uint8_t *data, size_t len, int key)
{
    struct sockaddr_in to[PORTS];
    uint8_t buf[REMOTE_MAX_DATAGRAM];
    RemoteMsg m;
    size_t off;
    int n = 0, i;

    if (!H.running || !len || len > 0xFFFFu * (size_t)REMOTE_VIDEO_CHUNK)
        return;
    SDL_LockMutex(H.lock);
    for (i = 1; i < PORTS; i++)
        if (H.players[i].in_use)
            to[n++] = H.players[i].from;
    SDL_UnlockMutex(H.lock);
    memset(&m, 0, sizeof m);
    m.type = REMOTE_VIDEO;
    m.frame = frame;
    m.count = (uint16_t)((len + REMOTE_VIDEO_CHUNK - 1) / REMOTE_VIDEO_CHUNK);
    m.flags = key ? REMOTE_VIDEO_KEY : 0;
    for (off = 0; off < len; off += REMOTE_VIDEO_CHUNK) {
        size_t k;
        m.length = (uint16_t)(len - off < REMOTE_VIDEO_CHUNK ? len - off : REMOTE_VIDEO_CHUNK);
        m.data = data + off;
        k = remote_encode(&m, buf, sizeof buf);
        for (i = 0; i < n; i++)
            sendto(H.sock, (const char *)buf, (int)k, 0, (const struct sockaddr *)&to[i],
                   sizeof to[i]);
        m.index++;
    }
}

void recomp_remote_rumble(unsigned port, unsigned short low, unsigned short high)
{
    RemoteMsg m;
    struct sockaddr_in to;
    int held = 0;

    if (!H.running || port == 0 || port >= PORTS)
        return;
    SDL_LockMutex(H.lock);
    if (H.players[port].in_use) {
        to = H.players[port].from;
        held = 1;
    }
    SDL_UnlockMutex(H.lock);
    if (!held)
        return;
    memset(&m, 0, sizeof m);
    m.type = REMOTE_RUMBLE;
    m.low = low;
    m.high = high;
    send_to(&to, &m);
}

const char *recomp_remote_name(unsigned port)
{
    static char name[PORTS][25];

    if (!H.running || port == 0 || port >= PORTS)
        return "";
    SDL_LockMutex(H.lock);
    snprintf(name[port], sizeof name[port], "%s",
             H.players[port].in_use ? H.players[port].name : "");
    SDL_UnlockMutex(H.lock);
    return name[port];
}
