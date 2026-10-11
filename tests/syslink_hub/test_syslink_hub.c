/*
 * test_syslink_hub.c -- the System Link switch, with no game and no router.
 *
 * One process opens a host and several joining links on 127.0.0.1, as
 * several consoles would on several machines, and checks who gets which
 * frame: a broadcast reaches everyone but its sender, a frame for one
 * console reaches that console alone, a wrong join code or a copy of the
 * host's own identity is refused and reaches nobody, and a console that
 * leaves is gone at once. The pair mode (xemu's bare-frame format) is
 * checked the same way.
 *
 * Only the loopback address is bound, so nothing asks the firewall and
 * nothing leaves the machine; nothing here asks a router anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "xbox_net_udp.h"

static int g_fail;

#define CHECK(cond, what) do { \
    if (cond) printf("  ok    %s\n", what); \
    else { printf("  FAIL  %s\n", what); g_fail++; } \
} while (0)

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t MAC_H[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x10 };
static const uint8_t MAC_1[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x11 };
static const uint8_t MAC_2[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x12 };
static const uint8_t MAC_3[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x13 };
static const uint8_t MAC_4[6] = { 0x00, 0x50, 0xF2, 0x00, 0x00, 0x14 };

static uint32_t frame(uint8_t *f, const uint8_t dst[6], const uint8_t src[6], uint8_t tag)
{
    memset(f, 0, 64);
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = 0x08;                   /* IPv4, not that anything reads it */
    f[14] = tag;
    return 64;
}

/* The tag of the next frame delivered to l within ms, or -1. Takes it. */
static int next_tag(XboxNetLink *l, int ms)
{
    uint8_t buf[XBOX_NET_FRAME_MAX];
    int waited;

    for (waited = 0; waited <= ms; waited += 5) {
        uint32_t n = xbox_net_link_peek(l, buf, sizeof buf);
        if (n) {
            xbox_net_link_pop(l);
            return n > 14 ? buf[14] : -1;
        }
        sleep_ms(5);
    }
    return -1;
}

static int wait_joined(XboxNetLink *l, int ms)
{
    int waited;

    for (waited = 0; waited <= ms; waited += 10) {
        if (xbox_net_link_joined(l))
            return 1;
        sleep_ms(10);
    }
    return 0;
}

static int wait_consoles(XboxNetLink *host, int want, int ms)
{
    int waited;

    for (waited = 0; waited <= ms; waited += 10) {
        if (xbox_net_link_consoles(host) == want)
            return 1;
        sleep_ms(10);
    }
    return 0;
}

static void test_route(void)
{
    uint8_t macs[3][6];
    int has[3] = { 1, 1, 0 }, to[16], local, n;

    printf("routing:\n");
    memcpy(macs[0], MAC_1, 6);
    memcpy(macs[1], MAC_2, 6);
    memcpy(macs[2], MAC_3, 6);

    n = xbox_net_route(BCAST, MAC_H, (const uint8_t (*)[6])macs, has, 3, 0, &local, to);
    CHECK(n == 2 && to[0] == 1 && to[1] == 2 && local,
          "a broadcast from a peer goes to the host and every other peer");
    n = xbox_net_route(BCAST, MAC_H, (const uint8_t (*)[6])macs, has, 3, -1, &local, to);
    CHECK(n == 3 && !local, "a broadcast from the host goes to every peer, not back");
    n = xbox_net_route(MAC_2, MAC_H, (const uint8_t (*)[6])macs, has, 3, 0, &local, to);
    CHECK(n == 1 && to[0] == 1 && !local, "a frame for a known peer goes to it alone");
    n = xbox_net_route(MAC_H, MAC_H, (const uint8_t (*)[6])macs, has, 3, 2, &local, to);
    CHECK(n == 0 && local, "a frame for the host goes to the host alone");
    n = xbox_net_route(MAC_4, MAC_H, (const uint8_t (*)[6])macs, has, 3, 1, &local, to);
    CHECK(n == 2 && to[0] == 0 && to[1] == 2 && !local,
          "an address not seen yet floods to every peer but the sender");
}

static XboxNetLink *open_link(XboxNetMode mode, const char *remote, int port,
                              const char *code, const uint8_t mac[6])
{
    XboxNetConfig c;

    memset(&c, 0, sizeof c);
    c.mode = mode;
    if (remote)
        snprintf(c.remote, sizeof c.remote, "%s", remote);
    c.port = port;
    if (code)
        snprintf(c.code, sizeof c.code, "%s", code);
    c.loopback_only = 1;
    c.quiet = 1;
    return xbox_net_link_open(&c, mac);
}

static void test_session(void)
{
    XboxNetLink *h, *j1, *j2, *j3, *bad, *same;
    char addr[64];
    uint8_t f[64];
    uint32_t n;

    printf("a hosted session on 127.0.0.1:\n");
    h = open_link(XBOX_NET_HOST, NULL, 0, "Siberia", MAC_H);
    CHECK(h != NULL, "the host listens");
    if (!h)
        return;
    snprintf(addr, sizeof addr, "127.0.0.1:%d", xbox_net_link_port(h));

    j1 = open_link(XBOX_NET_JOIN, addr, 0, "siberia", MAC_1);   /* any case */
    j2 = open_link(XBOX_NET_JOIN, addr, 0, "Siberia", MAC_2);
    j3 = open_link(XBOX_NET_JOIN, addr, 0, "Siberia", MAC_3);
    CHECK(j1 && j2 && j3, "three consoles open their links");
    if (!j1 || !j2 || !j3)
        return;
    CHECK(wait_joined(j1, 3000) && wait_joined(j2, 3000) && wait_joined(j3, 3000),
          "all three are welcomed");
    CHECK(wait_consoles(h, 4, 1000), "the host counts four consoles");
    /* Each heard the count when it was welcomed, so the first may have
     * heard two; what matters is that it heard one. */
    CHECK(xbox_net_link_consoles(j1) >= 2, "a joined console hears a count from the host");

    /* A broadcast from console 1: everyone else, not console 1. */
    n = frame(f, BCAST, MAC_1, 0xB1);
    xbox_net_link_send(j1, f, n);
    CHECK(next_tag(h, 1000) == 0xB1, "broadcast from 1 reaches the host");
    CHECK(next_tag(j2, 1000) == 0xB1, "broadcast from 1 reaches 2");
    CHECK(next_tag(j3, 1000) == 0xB1, "broadcast from 1 reaches 3");
    CHECK(next_tag(j1, 200) == -1, "broadcast from 1 does not come back to 1");

    /* Console 2 to console 3 only: the host knows 3's address from its
     * hello, so nobody else hears it. */
    n = frame(f, MAC_3, MAC_2, 0x23);
    xbox_net_link_send(j2, f, n);
    CHECK(next_tag(j3, 1000) == 0x23, "a frame from 2 for 3 reaches 3");
    CHECK(next_tag(j1, 200) == -1, "... and not 1");
    CHECK(next_tag(h, 200) == -1, "... and not the host");

    /* The host's own card to console 1. */
    n = frame(f, MAC_1, MAC_H, 0x41);
    xbox_net_link_send(h, f, n);
    CHECK(next_tag(j1, 1000) == 0x41, "a frame from the host for 1 reaches 1");
    CHECK(next_tag(j2, 200) == -1 && next_tag(j3, 50) == -1, "... and nobody else");

    /* Console 3 answers the host. */
    n = frame(f, MAC_H, MAC_3, 0x34);
    xbox_net_link_send(j3, f, n);
    CHECK(next_tag(h, 1000) == 0x34, "a frame from 3 for the host reaches the host");
    CHECK(next_tag(j1, 200) == -1 && next_tag(j2, 50) == -1, "... and nobody else");

    /* The wrong code is refused, and what it sends reaches nobody. */
    bad = open_link(XBOX_NET_JOIN, addr, 0, "guess", MAC_4);
    CHECK(bad != NULL, "a console with the wrong code opens its link");
    if (bad) {
        CHECK(!wait_joined(bad, 1500), "... and is not welcomed");
        n = frame(f, BCAST, MAC_4, 0xEE);
        xbox_net_link_send(bad, f, n);
        CHECK(next_tag(h, 300) == -1 && next_tag(j1, 50) == -1,
              "... and nothing it sends reaches anyone");
        CHECK(xbox_net_link_consoles(h) == 4, "... and the host still counts four");
        xbox_net_link_close(bad);
    }

    /* A copy with the host's own identity -- two copies sharing one save
     * folder -- would make the switch deliver to the wrong card. */
    same = open_link(XBOX_NET_JOIN, addr, 0, "Siberia", MAC_H);
    CHECK(same != NULL && !wait_joined(same, 1500),
          "a console with the host's own address is refused");
    if (same)
        xbox_net_link_close(same);

    /* Leaving says goodbye; the host does not wait for silence. */
    xbox_net_link_close(j3);
    CHECK(wait_consoles(h, 3, 1000), "a console that leaves is gone at once");
    n = frame(f, BCAST, MAC_2, 0xB2);
    xbox_net_link_send(j2, f, n);
    CHECK(next_tag(h, 1000) == 0xB2 && next_tag(j1, 1000) == 0xB2,
          "the session carries on without it");

    xbox_net_link_close(j1);
    xbox_net_link_close(j2);
    xbox_net_link_close(h);
}

static void test_pair(void)
{
    XboxNetLink *a, *b;
    char to_b[64], to_a[64];
    uint8_t f[64];
    uint32_t n;
    XboxNetConfig c;

    printf("a pair (xemu's format):\n");
    /* b first on any port, then a pointed at it, then b pointed at a: two
     * links that know each other's ports, as two .bat files would. */
    memset(&c, 0, sizeof c);
    c.mode = XBOX_NET_PAIR;
    c.loopback_only = 1;
    c.quiet = 1;
    snprintf(c.remote, sizeof c.remote, "127.0.0.1:9");     /* replaced below */
    a = xbox_net_link_open(&c, MAC_1);
    CHECK(a != NULL, "a opens");
    if (!a)
        return;
    snprintf(to_a, sizeof to_a, "127.0.0.1:%d", xbox_net_link_port(a));
    snprintf(c.remote, sizeof c.remote, "%s", to_a);
    b = xbox_net_link_open(&c, MAC_2);
    CHECK(b != NULL, "b opens, sending to a");
    if (!b) {
        xbox_net_link_close(a);
        return;
    }
    snprintf(to_b, sizeof to_b, "127.0.0.1:%d", xbox_net_link_port(b));
    xbox_net_link_close(a);
    snprintf(c.remote, sizeof c.remote, "%s", to_b);
    c.port = atoi(strrchr(to_a, ':') + 1);
    a = xbox_net_link_open(&c, MAC_1);
    CHECK(a != NULL, "a reopens on its port, sending to b");
    if (!a) {
        xbox_net_link_close(b);
        return;
    }
    n = frame(f, BCAST, MAC_2, 0x51);
    xbox_net_link_send(b, f, n);
    CHECK(next_tag(a, 1000) == 0x51, "b to a");
    n = frame(f, MAC_2, MAC_1, 0x52);
    xbox_net_link_send(a, f, n);
    CHECK(next_tag(b, 1000) == 0x52, "a to b");
    xbox_net_link_close(a);
    xbox_net_link_close(b);
}

int main(void)
{
    test_route();
    test_session();
    test_pair();
    printf(g_fail ? "%d check(s) failed\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
