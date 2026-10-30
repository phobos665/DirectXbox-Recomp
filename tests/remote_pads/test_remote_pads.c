/*
 * Remote play's host (src/input/remote_pads.c) on 127.0.0.1, driven by raw
 * datagrams in its wire format: no game, no window, no pad.
 */
#include "remote_pads.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define sock_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int sock_t;
#  define sock_close close
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct sockaddr_in g_host;

static sock_t joiner(void)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    DWORD tv = 500;
#else
    struct timeval tv = { 0, 500000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    return s;
}

static void send_msg(sock_t s, const RemoteMsg *m)
{
    uint8_t buf[64];
    size_t n = remote_encode(m, buf, sizeof buf);
    sendto(s, (const char *)buf, (int)n, 0, (struct sockaddr *)&g_host, sizeof g_host);
}

/* The next message for this joiner, 0 if none came within half a second. */
static int recv_msg(sock_t s, RemoteMsg *m)
{
    uint8_t buf[128];
    int n = (int)recv(s, (char *)buf, sizeof buf, 0);
    return n > 0 && remote_decode(buf, (size_t)n, m, NULL);
}

static int hello(sock_t s, const char *code, const char *name, RemoteMsg *reply)
{
    RemoteMsg m;
    memset(&m, 0, sizeof m);
    m.type = REMOTE_HELLO;
    snprintf(m.code, sizeof m.code, "%s", code);
    snprintf(m.name, sizeof m.name, "%s", name);
    send_msg(s, &m);
    return recv_msg(s, reply);
}

static void pad(sock_t s, uint32_t seq, uint16_t buttons, int16_t lx)
{
    RemoteMsg m;
    memset(&m, 0, sizeof m);
    m.type = REMOTE_PAD;
    m.seq = seq;
    m.pad.wButtons = buttons;
    m.pad.bAnalogButtons[0] = 200;
    m.pad.sThumbLX = lx;
    send_msg(s, &m);
}

static unsigned g_local;
static unsigned local_mask(void) { return g_local; }

int main(void)
{
    RemoteMsg r;
    XBOX_GAMEPAD p;
    sock_t a, b, c, d;
    int port = 0;
    uint8_t buf[64];

    /* The wire on its own: a PAD survives the round trip. */
    {
        RemoteMsg m, back;
        size_t n;
        memset(&m, 0, sizeof m);
        m.type = REMOTE_PAD;
        m.seq = 0x01020304u;
        m.pad.wButtons = 0x1234;
        m.pad.bAnalogButtons[7] = 9;
        m.pad.sThumbLX = -32768;
        m.pad.sThumbRY = 32767;
        n = remote_encode(&m, buf, sizeof buf);
        CHECK(n == 28, "PAD is %u bytes", (unsigned)n);
        CHECK(remote_decode(buf, n, &back, NULL) && back.seq == m.seq &&
              back.pad.wButtons == 0x1234 && back.pad.bAnalogButtons[7] == 9 &&
              back.pad.sThumbLX == -32768 && back.pad.sThumbRY == 32767, "PAD round trip");
        CHECK(!remote_decode(buf, 20, &back, NULL), "a short PAD is refused");
        buf[0] ^= 1;
        CHECK(!remote_decode(buf, n, &back, NULL), "a wrong magic is refused");
    }

    SDL_Init(0);
    recomp_remote_set_local_mask_fn(local_mask);
    g_local = 1u << 1;                          /* the host has its own pad on port 2 */
    CHECK(recomp_remote_host_start(0, "duck", &port) == 0 && port > 0, "host starts");
    memset(&g_host, 0, sizeof g_host);
    g_host.sin_family = AF_INET;
    g_host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_host.sin_port = htons((uint16_t)port);

    a = joiner(); b = joiner(); c = joiner(); d = joiner();

    CHECK(hello(a, "goose", "wrong", &r) && r.type == REMOTE_REFUSED &&
          r.reason == REMOTE_REFUSED_CODE, "a wrong code is refused");
    CHECK(recomp_remote_mask() == 0, "nobody is in after a refusal");

    CHECK(hello(a, "duck", "Ann", &r) && r.type == REMOTE_WELCOME && r.port == 3,
          "the first joiner gets port 3, the host holding port 2 (got %d)", r.port);
    CHECK(hello(b, "duck", "Bob", &r) && r.type == REMOTE_WELCOME && r.port == 4,
          "the second gets port 4 (got %d)", r.port);
    CHECK(hello(c, "duck", "Cat", &r) && r.type == REMOTE_REFUSED &&
          r.reason == REMOTE_REFUSED_FULL, "a third is told the game is full");
    CHECK(hello(a, "duck", "Ann", &r) && r.type == REMOTE_WELCOME && r.port == 3,
          "a second hello keeps the same port");
    CHECK(recomp_remote_mask() == ((1u << 2) | (1u << 3)), "mask 0x%X", recomp_remote_mask());
    CHECK(strcmp(recomp_remote_name(2), "Ann") == 0, "name '%s'", recomp_remote_name(2));

    pad(a, 5, 0x0010, 1000);
    pad(a, 4, 0x0020, -1000);                   /* late: says less than seq 5 */
    SDL_Delay(100);
    CHECK(recomp_remote_sample(2, &p) && p.wButtons == 0x0010 && p.sThumbLX == 1000 &&
          p.bAnalogButtons[0] == 200, "newest wins (buttons 0x%X lx %d)", p.wButtons, p.sThumbLX);
    CHECK(!recomp_remote_sample(1, &p), "port 2 is the host's own");

    recomp_remote_rumble(2, 1234, 4321);
    CHECK(recv_msg(a, &r) && r.type == REMOTE_RUMBLE && r.low == 1234 && r.high == 4321,
          "rumble goes back to its player");

    memset(&r, 0, sizeof r);
    r.type = REMOTE_PING;
    r.stamp = 77;
    send_msg(d, &r);
    CHECK(recv_msg(d, &r) && r.type == REMOTE_PONG && r.stamp == 77, "a ping is answered");

    /* A joiner from another version is told so. */
    {
        RemoteMsg m;
        size_t n;
        memset(&m, 0, sizeof m);
        m.type = REMOTE_HELLO;
        n = remote_encode(&m, buf, sizeof buf);
        buf[4] = REMOTE_VERSION + 1;
        sendto(d, (const char *)buf, (int)n, 0, (struct sockaddr *)&g_host, sizeof g_host);
        CHECK(recv_msg(d, &r) && r.type == REMOTE_REFUSED && r.reason == REMOTE_REFUSED_VERSION,
              "another version is refused");
    }

    memset(&r, 0, sizeof r);
    r.type = REMOTE_BYE;
    send_msg(b, &r);
    SDL_Delay(100);
    CHECK(recomp_remote_mask() == (1u << 2), "a goodbye frees the port (mask 0x%X)",
          recomp_remote_mask());
    CHECK(hello(c, "duck", "Cat", &r) && r.type == REMOTE_WELCOME && r.port == 4,
          "and the next joiner gets it");

    recomp_remote_host_stop();
    sock_close(a); sock_close(b); sock_close(c); sock_close(d);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("remote_pads: all checks passed\n");
    return 0;
}
