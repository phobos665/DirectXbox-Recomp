/**
 * net_upnp.c -- see net_upnp.h.
 *
 * The forward is asked for with a one-hour lease and renewed every half
 * hour, so a copy that dies without tidying up (a crash, a closed window
 * that exits the process at once) leaves the router's table as it found it
 * within the hour. A router that only takes permanent forwards gets one,
 * removed when the process exits normally.
 */
#include "net_upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#endif

#ifdef XBOXRECOMP_HAVE_UPNP
#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"
#endif

#define LEASE_SECONDS   "3600"
#define RENEW_EVERY_MS  (30u * 60u * 1000u)
#define DISCOVER_MS     2000

static int sockets_up(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return 1;
#endif
}

static void sockets_down(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

int net_local_address(char *out, size_t n)
{
    struct sockaddr_in to, me;
    socklen_t ml = sizeof me;
    int ok = 0;
#ifdef _WIN32
    SOCKET s;
#else
    int s;
#endif

    if (!out || n == 0 || !sockets_up())
        return 0;
    out[0] = 0;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s != INVALID_SOCKET) {
#else
    if (s >= 0) {
#endif
        /* Connecting a UDP socket sends nothing; it only picks the route,
         * and with it the address this machine would be seen from. */
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
        if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 &&
            getsockname(s, (struct sockaddr *)&me, &ml) == 0 &&
            me.sin_addr.s_addr != htonl(INADDR_ANY)) {
            ok = inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n) != NULL;
        }
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
    sockets_down();
    return ok;
}

int net_address_is_private(const char *dotted)
{
    unsigned a = 0, b = 0, c = 0, d = 0;

    if (!dotted || sscanf(dotted, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 1;
    return a == 10 || a == 127 || a == 0 ||
           (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168) ||
           (a == 169 && b == 254) ||
           (a == 100 && b >= 64 && b <= 127);
}

#ifdef XBOXRECOMP_HAVE_UPNP

/* The router, if one answers: 1 with urls and data filled (the caller frees
 * urls), 0 if none. */
static int find_router(struct UPNPUrls *urls, struct IGDdatas *data,
                       char *lan, size_t ln, char *wan, size_t wn, int *kind)
{
    int err = 0;
    struct UPNPDev *devs = upnpDiscover(DISCOVER_MS, NULL, NULL, UPNP_LOCAL_PORT_ANY,
                                        0, 2, &err);

    *kind = UPNP_NO_IGD;
    if (!devs)
        return 0;
    *kind = UPNP_GetValidIGD(devs, urls, data, lan, (int)ln, wan, (int)wn);
    freeUPNPDevlist(devs);
    if (*kind == UPNP_NO_IGD)
        return 0;
    if (*kind != UPNP_CONNECTED_IGD && *kind != UPNP_PRIVATEIP_IGD) {
        FreeUPNPUrls(urls);
        return 0;
    }
    return 1;
}

NetUpnpState net_upnp_probe(NetUpnpResult *out)
{
    struct UPNPUrls urls;
    struct IGDdatas data;
    char wan[64] = "";
    int kind;

    memset(out, 0, sizeof *out);
    memset(&urls, 0, sizeof urls);
    memset(&data, 0, sizeof data);
    if (!sockets_up()) {
        out->state = NET_UPNP_NO_ROUTER;
        return out->state;
    }
    if (!find_router(&urls, &data, out->lan, sizeof out->lan, wan, sizeof wan, &kind)) {
        out->state = NET_UPNP_NO_ROUTER;
        snprintf(out->detail, sizeof out->detail,
                 "The router did not answer (UPnP off?). Friends on the internet need "
                 "the port forwarded by hand, or a virtual network.");
        sockets_down();
        return out->state;
    }
    if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype,
                                  out->external) != UPNPCOMMAND_SUCCESS)
        snprintf(out->external, sizeof out->external, "%s", wan);
    if (kind == UPNP_PRIVATEIP_IGD || net_address_is_private(out->external)) {
        out->state = NET_UPNP_BEHIND_ANOTHER;
        snprintf(out->detail, sizeof out->detail,
                 "The router's own internet address (%s) is private: another router "
                 "or your ISP is in front of it. Use a virtual network.",
                 out->external[0] ? out->external : "none");
    } else {
        out->state = NET_UPNP_FOUND;
    }
    FreeUPNPUrls(&urls);
    sockets_down();
    return out->state;
}

static struct {
    int  port;
    struct UPNPUrls urls;
    struct IGDdatas data;
    char lan[64];
    int  permanent;             /* the router took only a permanent forward */
    int  open;
} s_fwd;

static int add_forward(void)
{
    char port[16];
    int r;

    snprintf(port, sizeof port, "%d", s_fwd.port);
    r = UPNP_AddPortMapping(s_fwd.urls.controlURL, s_fwd.data.first.servicetype,
                            port, port, s_fwd.lan, "xboxrecomp System Link", "UDP",
                            NULL, s_fwd.permanent ? "0" : LEASE_SECONDS);
    if (r == 725 && !s_fwd.permanent) {         /* OnlyPermanentLeasesSupported */
        s_fwd.permanent = 1;
        r = UPNP_AddPortMapping(s_fwd.urls.controlURL, s_fwd.data.first.servicetype,
                                port, port, s_fwd.lan, "xboxrecomp System Link", "UDP",
                                NULL, "0");
    }
    return r;
}

static void remove_forward(void)
{
    char port[16];

    if (!s_fwd.open)
        return;
    snprintf(port, sizeof port, "%d", s_fwd.port);
    UPNP_DeletePortMapping(s_fwd.urls.controlURL, s_fwd.data.first.servicetype,
                           port, "UDP", NULL);
    s_fwd.open = 0;
}

static void say(const char *line)
{
    fprintf(stderr, "[NET] %s\n", line);
    fflush(stderr);
}

static void open_and_keep(void)
{
    char wan[64] = "", ext[64] = "", line[320];
    int kind, r;

    if (!sockets_up())
        return;
    if (!find_router(&s_fwd.urls, &s_fwd.data, s_fwd.lan, sizeof s_fwd.lan,
                     wan, sizeof wan, &kind)) {
        snprintf(line, sizeof line,
                 "the router did not answer UPnP: friends on the internet need UDP "
                 "port %d forwarded to this PC by hand, or a virtual network "
                 "(ZeroTier, Tailscale); on this network they can join already",
                 s_fwd.port);
        say(line);
        return;
    }
    if (UPNP_GetExternalIPAddress(s_fwd.urls.controlURL, s_fwd.data.first.servicetype,
                                  ext) != UPNPCOMMAND_SUCCESS)
        snprintf(ext, sizeof ext, "%s", wan);

    r = add_forward();
    if (r != UPNPCOMMAND_SUCCESS) {
        if (r == 718)                           /* ConflictInMappingEntry */
            snprintf(line, sizeof line,
                     "the router already forwards UDP port %d to another machine; "
                     "choose another port in the launcher", s_fwd.port);
        else
            snprintf(line, sizeof line,
                     "the router refused to forward UDP port %d (%d: %s); friends on "
                     "the internet need it forwarded by hand", s_fwd.port, r,
                     strupnperror(r) ? strupnperror(r) : "no description");
        say(line);
        FreeUPNPUrls(&s_fwd.urls);
        return;
    }
    s_fwd.open = 1;
    atexit(remove_forward);
    if (kind == UPNP_PRIVATEIP_IGD || net_address_is_private(ext))
        snprintf(line, sizeof line,
                 "the router forwards UDP port %d here, but its own internet address "
                 "(%s) is private: another router or the ISP is in front of it, so "
                 "friends on the internet still cannot reach this PC. Use a virtual "
                 "network (ZeroTier, Tailscale)", s_fwd.port, ext[0] ? ext : "none");
    else
        snprintf(line, sizeof line,
                 "the router forwards UDP port %d here%s: friends on the internet "
                 "join %s:%d", s_fwd.port, s_fwd.permanent ? " (until the game exits)" : "",
                 ext, s_fwd.port);
    say(line);

    /* Renewed while the process lives; the thread dies with it. */
    for (;;) {
#ifdef _WIN32
        Sleep(RENEW_EVERY_MS);
#else
        sleep(RENEW_EVERY_MS / 1000u);  /* usleep need not take a second or more */
#endif
        if (!s_fwd.permanent && add_forward() != UPNPCOMMAND_SUCCESS)
            say("the router stopped renewing the port forward; friends already in "
                "stay in, new ones from the internet may not get through");
    }
}

#ifdef _WIN32
static DWORD WINAPI open_thread(LPVOID unused) { (void)unused; open_and_keep(); return 0; }
#else
static void *open_thread(void *unused) { (void)unused; open_and_keep(); return NULL; }
#endif

void net_upnp_open_async(int port)
{
    static int started;

    if (started || port <= 0)
        return;
    started = 1;
    s_fwd.port = port;
#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, open_thread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, open_thread, NULL) == 0)
            pthread_detach(t);
    }
#endif
}

#else /* !XBOXRECOMP_HAVE_UPNP */

NetUpnpState net_upnp_probe(NetUpnpResult *out)
{
    memset(out, 0, sizeof *out);
    out->state = NET_UPNP_NOT_BUILT;
    snprintf(out->detail, sizeof out->detail,
             "This build cannot open the router (no third_party/miniupnp).");
    return out->state;
}

void net_upnp_open_async(int port)
{
    fprintf(stderr, "[NET] this build cannot ask the router to open UDP port %d "
                    "(built without third_party/miniupnp)\n", port);
    fflush(stderr);
}

#endif /* XBOXRECOMP_HAVE_UPNP */
