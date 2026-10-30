/*
 * xboxrecomp-remote -- join a friend's game as controller 2, 3 or 4.
 *
 *   xboxrecomp-remote <host>[:port] [--code <text>] [--name <text>]
 *
 * The other half of remote play (remote_pads.h): the friend runs the title
 * with remote_play = host, this program sends them your pad. It reads your
 * controller 1 bindings, so whatever you play with locally -- a pad, the
 * keyboard, both -- is what you play with here. The window shows whether
 * you are in, as which controller, and the round trip; keep it in front
 * when you play on the keyboard. It holds no game code and reads no game
 * files, so it can go to anyone. The window shows the host's picture
 * (remote_stream.h) once it arrives, grey while asking and red when refused,
 * and its title says the rest. The sound does not come back yet.
 */
#include "remote_pads.h"
#include "remote_stream.h"
#include "input_bindings.h"

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
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_BAD (-1)
#  define sock_close close
#endif

#define SEND_MS   8         /* the pad, about 125 times a second */
#define HELLO_MS  1000      /* until the host answers            */
#define PING_MS   1000
#define KEYREQ_MS 250       /* at most this often, after a loss     */

static sock_t g_sock = SOCK_BAD;
static struct sockaddr_in g_host;

static void send_msg(const RemoteMsg *m)
{
    uint8_t buf[64];
    size_t n = remote_encode(m, buf, sizeof buf);
    if (n)
        sendto(g_sock, (const char *)buf, (int)n, 0, (const struct sockaddr *)&g_host,
               sizeof g_host);
}

static int resolve(const char *spec, struct sockaddr_in *out)
{
    char host[256];
    const char *colon = strrchr(spec, ':');
    int port = REMOTE_DEFAULT_PORT;
    struct addrinfo hints, *res = NULL;

    snprintf(host, sizeof host, "%s", spec);
    if (colon) {
        host[colon - spec] = '\0';
        port = atoi(colon + 1);
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return 0;
}

/* The picture, as big as the window allows with its own shape, centred. */
static void show(SDL_Window *win, const uint8_t *bgra, unsigned w, unsigned h)
{
    SDL_Surface *dst = SDL_GetWindowSurface(win), *src;
    SDL_Rect r;

    if (!dst)
        return;
    src = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_BGRA32, (void *)bgra, (int)w * 4);
    if (!src)
        return;
    if ((long long)dst->w * h > (long long)dst->h * w) {
        r.h = dst->h;
        r.w = (int)((long long)dst->h * w / h);
    } else {
        r.w = dst->w;
        r.h = (int)((long long)dst->w * h / w);
    }
    r.x = (dst->w - r.w) / 2;
    r.y = (dst->h - r.h) / 2;
    SDL_FillSurfaceRect(dst, NULL, SDL_MapSurfaceRGB(dst, 0, 0, 0));
    SDL_BlitSurfaceScaled(src, NULL, dst, &r, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(src);
    SDL_UpdateWindowSurface(win);
}

/* RECOMP_REMOTE_SHOT=<file.bmp>: the last picture, written on the way out,
 * for checking a run with no window to look at. */
static void save_shot(const char *path, const uint8_t *bgra, unsigned w, unsigned h)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_BGRA32,
                                           (void *)bgra, (int)w * 4);
    if (s && SDL_SaveBMP(s, path))
        fprintf(stderr, "[REMOTE] last picture written to %s\n", path);
    SDL_DestroySurface(s);
}

static void usage(void)
{
    fprintf(stderr, "usage: xboxrecomp-remote <host>[:port] [--code <text>] [--name <text>]\n"
                    "  joins a friend's game (remote_play = host) as controller 2, 3 or 4;\n"
                    "  port %d unless they chose another\n", REMOTE_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    const char *spec = NULL, *code = "", *name = NULL;
    char status[128] = "asking to join...", title[160];
    SDL_Window *win;
    RemoteMsg m;
    uint64_t next_send = 0, next_hello = 0, next_ping = 0, last_heard = 0, last_keyreq = 0;
    uint32_t seq = 0, rtt = 0;
    int port = 0, running = 1, i, pictures = 0, lost_frames = 0;
    RemoteAssembler *asmb;
    RemoteDecoder *dec;
    const uint8_t *last_pic = NULL;
    unsigned last_w = 0, last_h = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--code") && i + 1 < argc)
            code = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else if (argv[i][0] != '-' && !spec)
            spec = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (!spec) {
        usage();
        return 2;
    }
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return 1;
    }
#endif
    if (resolve(spec, &g_host) != 0) {
        fprintf(stderr, "cannot find %s\n", spec);
        return 1;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == SOCK_BAD)
        return 1;
#ifdef _WIN32
    {
        u_long on = 1;
        ioctlsocket(g_sock, FIONBIO, &on);
    }
#else
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    /* This program reads the bindings but must never host. */
    recomp_remote_never_host();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    /* No SDL renderer: the toolkit's SDL is built without one outside Linux.
     * The window's own surface is enough for a colour. */
    if (!(win = SDL_CreateWindow("Remote play", 960, 720, SDL_WINDOW_RESIZABLE)))
        fprintf(stderr, "[REMOTE] no window (%s): a pad still works, the keyboard "
                "does not\n", SDL_GetError());
    recomp_bindings_init();
    asmb = remote_assembler_create();
    dec = remote_decoder_create();
    if (!dec)
        fprintf(stderr, "[REMOTE] no H.264 decoder (FFmpeg): your pad works, but you will "
                "not see the picture\n");
    if (!name || !*name)
        name = SDL_getenv("USER") ? SDL_getenv("USER") : SDL_getenv("USERNAME");

    while (running) {
        SDL_Event ev;
        uint64_t now = SDL_GetTicks();
        uint8_t buf[REMOTE_MAX_DATAGRAM];
        int n;

        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT)
                running = 0;

        while ((n = (int)recv(g_sock, (char *)buf, sizeof buf, 0)) > 0) {
            int version;
            if (!remote_decode(buf, (size_t)n, &m, &version))
                continue;
            last_heard = now;
            if (m.type == REMOTE_WELCOME) {
                if (port != m.port)
                    fprintf(stderr, "[REMOTE] in: controller %d\n", m.port);
                port = m.port;
            } else if (m.type == REMOTE_REFUSED && m.reason == REMOTE_REFUSED_UNKNOWN) {
                if (port > 0)
                    fprintf(stderr, "[REMOTE] the host lost track of us; asking again\n");
                port = 0;                      /* say hello again */
                next_hello = 0;
            } else if (m.type == REMOTE_REFUSED) {
                snprintf(status, sizeof status, "%s",
                         m.reason == REMOTE_REFUSED_FULL ? "the game is full (ports 2-4 taken)"
                       : m.reason == REMOTE_REFUSED_CODE ? "wrong code"
                                                          : "the host runs another version");
                fprintf(stderr, "[REMOTE] refused: %s\n", status);
                port = -1;
            } else if (m.type == REMOTE_RUMBLE) {
                recomp_bindings_rumble(0, m.low, m.high);
            } else if (m.type == REMOTE_PONG) {
                rtt = (uint32_t)now - m.stamp;
            } else if (m.type == REMOTE_VIDEO && asmb) {
                const uint8_t *f;
                size_t flen;
                int lost;
                if (remote_assembler_piece(asmb, m.frame, m.index, m.count,
                                           (m.flags & REMOTE_VIDEO_KEY) != 0, m.data, m.length,
                                           &f, &flen, &lost) && dec) {
                    const uint8_t *bgra;
                    unsigned w, h;
                    int r = remote_decoder_frame(dec, f, flen, &bgra, &w, &h);
                    if (r > 0) {
                        if (win)
                            show(win, bgra, w, h);
                        if (pictures++ == 0)
                            fprintf(stderr, "[REMOTE] picture: %ux%u\n", w, h);
                        last_pic = bgra;
                        last_w = w;
                        last_h = h;
                    } else if (r < 0) {
                        lost = 1;
                    }
                }
                if (lost) {
                    lost_frames++;
                    if (now - last_keyreq >= KEYREQ_MS) {
                        RemoteMsg k;
                        memset(&k, 0, sizeof k);
                        k.type = REMOTE_KEYREQ;
                        send_msg(&k);
                        last_keyreq = now;
                    }
                }
            }
        }
        if (port > 0 && last_heard && now - last_heard > REMOTE_TIMEOUT_MS) {
            port = 0;                          /* the host went away: ask again */
            snprintf(status, sizeof status, "lost the host; asking again...");
        }

        if (port == 0 && now >= next_hello) {
            memset(&m, 0, sizeof m);
            m.type = REMOTE_HELLO;
            snprintf(m.code, sizeof m.code, "%s", code);
            snprintf(m.name, sizeof m.name, "%s", name ? name : "");
            send_msg(&m);
            next_hello = now + HELLO_MS;
        }
        if (port > 0 && now >= next_send) {
            memset(&m, 0, sizeof m);
            m.type = REMOTE_PAD;
            m.seq = ++seq;
            recomp_bindings_sample(0, &m.pad);
            send_msg(&m);
            next_send = now + SEND_MS;
        }
        if (now >= next_ping) {
            memset(&m, 0, sizeof m);
            m.type = REMOTE_PING;
            m.stamp = (uint32_t)now;
            send_msg(&m);
            next_ping = now + PING_MS;
        }

        if (port > 0)
            snprintf(status, sizeof status, "controller %d - round trip %u ms%s", port, rtt,
                     pictures ? "" : " - no picture yet");
        snprintf(title, sizeof title, "Remote play: %s", status);
        if (win) {
            SDL_Surface *sf = pictures ? NULL : SDL_GetWindowSurface(win);
            SDL_SetWindowTitle(win, title);
            if (sf) {                          /* until the picture comes */
                Uint32 c = port > 0 ? SDL_MapSurfaceRGB(sf, 40, 120, 60)
                         : port < 0 ? SDL_MapSurfaceRGB(sf, 140, 40, 40)
                                    : SDL_MapSurfaceRGB(sf, 60, 60, 60);
                SDL_FillSurfaceRect(sf, NULL, c);
                SDL_UpdateWindowSurface(win);
            }
        }
        SDL_Delay(2);
    }

    if (port > 0) {
        memset(&m, 0, sizeof m);
        m.type = REMOTE_BYE;
        send_msg(&m);
    }
    fprintf(stderr, "[REMOTE] %d picture(s) shown, %d frame(s) lost on the way\n",
            pictures, lost_frames);
    if (last_pic && SDL_getenv("RECOMP_REMOTE_SHOT"))
        save_shot(SDL_getenv("RECOMP_REMOTE_SHOT"), last_pic, last_w, last_h);
    remote_decoder_destroy(dec);
    remote_assembler_destroy(asmb);
    sock_close(g_sock);
    if (win)
        SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
