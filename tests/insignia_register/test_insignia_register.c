/*
 * Registering a console with Insignia (src/net/insignia_register.c): the
 * request body, what each answer means, and one request through the real
 * transport to a server on 127.0.0.1 standing in for Insignia. Never talks
 * to insignia.live: RECOMP_INSIGNIA_URL is set before anything is sent.
 * Made-up console data only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "insignia_register.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define close_sock closesocket
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char k_serial[12] = { '1','2','3','4','5','6','7','8','9','0','1','2' };
static const uint8_t k_hd[16] = { 0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,
                                  0x30,0x31,0x00,0x50,0xF2,0x00,0x00,0x01 };
static const uint8_t k_online[16] = { 0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                                      0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10 };
static const char k_body[] =
    "{\"serial_number\": \"123456789012\", \"hdd_key\": \"30303030303030303031005"
    "0F2000001\", \"online_key\": \"0123456789ABCDEFFEDCBA9876543210\"}";

static void test_body(void)
{
    static const uint8_t zero[16];
    char out[256], bad[12];

    CHECK(insignia_register_body(k_serial, k_hd, k_online, out, sizeof out), "body made");
    CHECK(strcmp(out, k_body) == 0, "body is '%s'", out);
    CHECK(!insignia_register_body(k_serial, zero, k_online, out, sizeof out),
          "a zero HDD key is never sent");
    memcpy(bad, k_serial, 12);
    bad[5] = 'A';
    CHECK(!insignia_register_body(bad, k_hd, k_online, out, sizeof out), "a serial with a letter");
    CHECK(!insignia_register_body(k_serial, k_hd, k_online, out, 40), "too small a buffer");
}

static void test_classify(void)
{
    InsigniaResult r;

    insignia_classify(200, "{}", &r);
    CHECK(r.outcome == INSIGNIA_REGISTERED, "200 registers");
    /* Laravel's validation answer, as the assistant handles it. */
    insignia_classify(422, "{\"message\":\"The serial number has already been taken.\","
                           "\"errors\":{\"serial_number\":[\"The serial number has "
                           "already been taken.\"]}}", &r);
    CHECK(r.outcome == INSIGNIA_REFUSED, "422 refuses");
    CHECK(strcmp(r.message, "The serial number has already been taken.") == 0,
          "422's words: '%s'", r.message);
    insignia_classify(422, "{\"errors\":{\"hdd_key\":[\"One.\",\"Two \\\"quoted\\\".\"],"
                           "\"online_key\":[\"Three.\"]}}", &r);
    CHECK(strcmp(r.message, "One. Two \"quoted\". Three.") == 0, "every message: '%s'", r.message);
    insignia_classify(422, "{\"message\":\"Only this.\"}", &r);
    CHECK(strcmp(r.message, "Only this.") == 0, "message when no errors: '%s'", r.message);
    insignia_classify(422, "not json", &r);
    CHECK(r.outcome == INSIGNIA_REFUSED && r.message[0], "422 without a body");
    insignia_classify(429, "", &r);
    CHECK(r.outcome == INSIGNIA_TOO_OFTEN, "429");
    insignia_classify(500, NULL, &r);
    CHECK(r.outcome == INSIGNIA_HTTP_ERROR && r.http_status == 500, "500");
}

/* ---- a stand-in for Insignia on 127.0.0.1 ---- */

static sock_t g_listen = INVALID_SOCKET;
static char g_request[8192];
static const char *g_answer;

#ifdef _WIN32
static DWORD WINAPI serve(LPVOID arg)
#else
static void *serve(void *arg)
#endif
{
    sock_t c;
    size_t have = 0;
    long want = -1;

    (void)arg;
    c = accept(g_listen, NULL, NULL);
    if (c == INVALID_SOCKET)
        return 0;
    /* The headers, then Content-Length bytes of body. */
    while (have + 1 < sizeof g_request) {
        char *end;
        int got = (int)recv(c, g_request + have, (int)(sizeof g_request - 1 - have), 0);
        if (got <= 0)
            break;
        have += (size_t)got;
        g_request[have] = 0;
        end = strstr(g_request, "\r\n\r\n");
        if (end && want < 0) {
            const char *cl = strstr(g_request, "Content-Length:");
            if (!cl)
                cl = strstr(g_request, "content-length:");
            want = cl ? (long)(end + 4 - g_request) + atol(cl + 15) : (long)(end + 4 - g_request);
        }
        if (want >= 0 && (long)have >= want)
            break;
    }
    send(c, g_answer, (int)strlen(g_answer), 0);
    close_sock(c);
    return 0;
}

static void test_transport(void)
{
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    InsigniaResult r;
    char url[128];
    const char *body;
#ifdef _WIN32
    HANDLE t;
#else
    pthread_t t;
#endif

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001u);
    a.sin_port = 0;
    if (g_listen == INVALID_SOCKET || bind(g_listen, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(g_listen, 1) != 0 || getsockname(g_listen, (struct sockaddr *)&a, &alen) != 0) {
        CHECK(0, "no loopback server");
        return;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/api/v1/xbox", ntohs(a.sin_port));
    set_env("RECOMP_INSIGNIA_URL", url);
    {
        static char answer[512];
        static const char json[] =
            "{\"message\":\"x\",\"errors\":{\"serial_number\":[\"The serial number has "
            "already been taken.\"]}}";
        snprintf(answer, sizeof answer, "HTTP/1.1 422 Unprocessable Content\r\n"
                 "Content-Type: application/json\r\nConnection: close\r\n"
                 "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(json), json);
        g_answer = answer;
    }
#ifdef _WIN32
    t = CreateThread(NULL, 0, serve, NULL, 0, NULL);
#else
    pthread_create(&t, NULL, serve, NULL);
#endif

    insignia_register(k_serial, k_hd, k_online, &r);
    if (r.outcome == INSIGNIA_NO_TRANSPORT) {
        /* Wake the server so it can finish, and say why nothing was tried. */
        sock_t c = socket(AF_INET, SOCK_STREAM, 0);
        connect(c, (struct sockaddr *)&a, sizeof a);
        close_sock(c);
        printf("insignia_register: no HTTPS transport here (%s); transport not tested\n",
               r.message);
    } else {
        CHECK(r.outcome == INSIGNIA_REFUSED, "the stand-in's 422 (outcome %d: %s)",
              r.outcome, r.message);
        CHECK(strcmp(r.message, "The serial number has already been taken.") == 0,
              "its words: '%s'", r.message);
    }
#ifdef _WIN32
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
    close_sock(g_listen);
    if (r.outcome == INSIGNIA_NO_TRANSPORT)
        return;

    CHECK(strncmp(g_request, "POST /api/v1/xbox HTTP/1.1\r\n", 28) == 0, "request line");
    CHECK(strstr(g_request, "Content-Type: application/json\r\n") != NULL, "content type");
    CHECK(strstr(g_request, "Accept: application/json\r\n") != NULL, "accept");
    CHECK(strstr(g_request, "User-Agent: DirectXbox-Recomp/") != NULL, "our own User-Agent");
    body = strstr(g_request, "\r\n\r\n");
    CHECK(body && strcmp(body + 4, k_body) == 0, "body sent: '%s'", body ? body + 4 : "");
}

static void test_marker(const char *dir)
{
    char path[600];

    snprintf(path, sizeof path, "%s/registered", dir);
    remove(path);
    CHECK(!insignia_is_registered(dir), "not registered yet");
    CHECK(insignia_mark_registered(dir), "marked");
    CHECK(insignia_is_registered(dir), "registered after");
    remove(path);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    /* First of all: nothing here may reach the real server. */
    set_env("RECOMP_INSIGNIA_URL", "http://127.0.0.1:9/none");
    set_env("RECOMP_INSIGNIA_UA", "");

    test_body();
    test_classify();
    test_transport();
    test_marker(argc > 1 ? argv[1] : ".");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("insignia_register: all checks passed\n");
    return 0;
}
