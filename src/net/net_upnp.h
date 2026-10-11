/**
 * net_upnp.h -- asking the router to let friends in, and saying what to tell
 * them.
 *
 * A System Link host has to be reachable from its friends' machines. On a
 * home network that means the router forwarding one UDP port to this PC,
 * which most routers will do when asked over UPnP. Shared by the runtime,
 * which opens the port while it hosts, and the launcher, which shows the
 * player which addresses to give out before the game starts.
 *
 * Built without UPnP (no third_party/miniupnp), the queries answer
 * NET_UPNP_NOT_BUILT and nothing is asked of any router.
 */
#ifndef XBOXRECOMP_NET_UPNP_H
#define XBOXRECOMP_NET_UPNP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This machine's address on the network it reaches the internet through,
 * dotted (e.g. 192.168.1.20). Found by asking the system which interface it
 * would route to a public address over -- no packet is sent. 0 if none. */
int net_local_address(char *out, size_t n);

typedef enum NetUpnpState {
    NET_UPNP_NOT_BUILT = 0,     /* this build has no UPnP                      */
    NET_UPNP_NO_ROUTER,         /* nothing answered: UPnP is off, or no router   */
    NET_UPNP_FOUND,             /* the router answered; external is its address  */
    NET_UPNP_BEHIND_ANOTHER,    /* its own internet address is private: a second
                                 * router, or the ISP's shared address (CGNAT),
                                 * sits in front, and opening this one is not
                                 * enough                                         */
    NET_UPNP_OPEN,              /* the port is forwarded here                    */
    NET_UPNP_REFUSED            /* the router said no; detail says why           */
} NetUpnpState;

typedef struct NetUpnpResult {
    NetUpnpState state;
    char external[64];          /* the router's internet address, if it said    */
    char lan[64];               /* this machine, as the router sees it          */
    char detail[160];           /* a sentence for the player when it went wrong */
} NetUpnpResult;

/* Find the router and its internet address; open nothing. Blocks for up to
 * about two seconds -- call it off the UI thread. Returns out->state. */
NetUpnpState net_upnp_probe(NetUpnpResult *out);

/* Ask the router to forward UDP `port` to this machine, on a thread of its
 * own, keep the forward renewed while the process lives, and say in the log
 * what happened and which address friends on the internet should use. */
void net_upnp_open_async(int port);

/* Whether a dotted IPv4 address is one nobody on the internet can reach:
 * private (10/8, 172.16/12, 192.168/16), the ISPs' shared 100.64/10,
 * loopback or link-local. */
int net_address_is_private(const char *dotted);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_NET_UPNP_H */
