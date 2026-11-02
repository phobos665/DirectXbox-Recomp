/**
 * Registering a console with Insignia. See insignia_register.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "insignia_register.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#define SEP "\\"
#else
#include <dlfcn.h>
#define SEP "/"
#endif

#ifndef XBOXRECOMP_VERSION
#define XBOXRECOMP_VERSION "dev"
#endif

#define REPLY_MAX 16384

/* ---- the request ------------------------------------------------------ */

static void hex16(const uint8_t k[16], char out[33])
{
    static const char digits[] = "0123456789ABCDEF";
    int i;

    for (i = 0; i < 16; i++) {
        out[2 * i] = digits[k[i] >> 4];
        out[2 * i + 1] = digits[k[i] & 15];
    }
    out[32] = 0;
}

int insignia_register_body(const char serial[12], const uint8_t hd_key[16],
                           const uint8_t online_key[16], char *out, size_t n)
{
    static const uint8_t zero[16];
    char sn[13], hd[33], ok[33];
    int i, len;

    for (i = 0; i < 12; i++)
        if (serial[i] < '0' || serial[i] > '9')
            return 0;
    if (memcmp(hd_key, zero, 16) == 0)
        return 0;                       /* Insignia refuses it, as does its assistant */
    memcpy(sn, serial, 12);
    sn[12] = 0;
    hex16(hd_key, hd);
    hex16(online_key, ok);
    len = snprintf(out, n, "{\"serial_number\": \"%s\", \"hdd_key\": \"%s\", "
                           "\"online_key\": \"%s\"}", sn, hd, ok);
    memset(hd, 0, sizeof hd);
    memset(ok, 0, sizeof ok);
    return len > 0 && (size_t)len < n;
}

/* ---- the answer ------------------------------------------------------- */

/* Just enough JSON for Laravel's error body: a string read at p (on its
 * opening quote) into out, escapes undone; returns the character after it. */
static const char *json_string(const char *p, char *out, size_t n)
{
    size_t k = 0;

    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            switch (c) {
            case 'n': c = ' '; break;
            case 't': c = ' '; break;
            case 'r': c = ' '; break;
            case 'b': case 'f': c = ' '; break;
            case 'u': {
                int i;
                for (i = 0; i < 4 && *p; i++)
                    p++;
                c = '?';                /* the messages are ASCII */
                break;
            }
            default: break;             /* \" \\ \/ */
            }
        }
        if (k + 1 < n)
            out[k++] = c;
    }
    if (n)
        out[k] = 0;
    return *p ? p + 1 : p;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* The value of top-level-or-nested `key`: a pointer to its first character,
 * or NULL. Good enough for an object that names the key once. */
static const char *json_find(const char *text, const char *key)
{
    char want[64], got[64];
    const char *p = text;

    snprintf(want, sizeof want, "%s", key);
    while ((p = strchr(p, '"')) != NULL) {
        const char *after = json_string(p, got, sizeof got);
        after = skip_ws(after);
        if (*after == ':' && strcmp(got, want) == 0)
            return skip_ws(after + 1);
        p = after;
    }
    return NULL;
}

static void append(char *out, size_t n, const char *s)
{
    size_t len = strlen(out);

    if (!*s || len + 2 >= n)
        return;
    snprintf(out + len, n - len, "%s%s", len ? " " : "", s);
}

/* Every string in the value at p that is not a key: the messages of an
 * "errors" object of arrays, or of an array, or one string. */
static void json_messages(const char *p, char *out, size_t n)
{
    char s[512];
    int depth = 0;

    do {
        if (*p == '"') {
            const char *after = json_string(p, s, sizeof s);
            if (*skip_ws(after) != ':')
                append(out, n, s);
            p = after;
            continue;
        }
        if (*p == '{' || *p == '[')
            depth++;
        else if (*p == '}' || *p == ']')
            depth--;
        p++;
    } while (*p && depth > 0);
}

void insignia_classify(int http_status, const char *body, InsigniaResult *r)
{
    char said[sizeof r->message];
    const char *v;

    said[0] = 0;
    if (body) {
        if ((v = json_find(body, "errors")) != NULL)
            json_messages(v, said, sizeof said);
        if (!said[0] && (v = json_find(body, "message")) != NULL && *v == '"')
            json_string(v, said, sizeof said);
    }
    r->http_status = http_status;
    if (http_status >= 200 && http_status < 300) {
        r->outcome = INSIGNIA_REGISTERED;
        snprintf(r->message, sizeof r->message, "Registered with Insignia.");
    } else if (http_status == 422) {
        r->outcome = INSIGNIA_REFUSED;
        snprintf(r->message, sizeof r->message, "%s",
                 said[0] ? said : "Insignia refused this console.");
    } else if (http_status == 429) {
        r->outcome = INSIGNIA_TOO_OFTEN;
        snprintf(r->message, sizeof r->message,
                 "Insignia has had too many requests from here; try again later.");
    } else {
        r->outcome = INSIGNIA_HTTP_ERROR;
        snprintf(r->message, sizeof r->message, "Insignia answered HTTP %d%s%s",
                 http_status, said[0] ? ": " : ".", said);
    }
}

/* ---- the transport ---------------------------------------------------- */

static const char *endpoint(void)
{
    const char *v = getenv("RECOMP_INSIGNIA_URL");
    return v && *v ? v : INSIGNIA_REGISTER_URL;
}

static const char *user_agent(void)
{
    const char *v = getenv("RECOMP_INSIGNIA_UA");
    return v && *v ? v : "DirectXbox-Recomp/" XBOXRECOMP_VERSION;
}

#ifdef _WIN32

static int post(const char *url, const char *body, int *status, char *reply,
                size_t reply_n, char *err, size_t err_n)
{
    wchar_t wurl[1024], wua[128], host[256], path[1024];
    URL_COMPONENTS uc;
    HINTERNET s = NULL, c = NULL, q = NULL;
    DWORD code = 0, len = sizeof code, avail, got;
    size_t have = 0;
    int ok = 0;

    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    MultiByteToWideChar(CP_UTF8, 0, user_agent(), -1, wua, 128);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(err, err_n, "not a URL: %s", url);
        return 0;
    }
    s = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (s)
        WinHttpSetTimeouts(s, 10000, 10000, 30000, 30000);
    c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    q = c ? WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
          : NULL;
    if (q && WinHttpSendRequest(q, L"Content-Type: application/json\r\n"
                                   L"Accept: application/json\r\n",
                                (DWORD)-1L, (LPVOID)body, (DWORD)strlen(body),
                                (DWORD)strlen(body), 0) &&
        WinHttpReceiveResponse(q, NULL) &&
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                            WINHTTP_NO_HEADER_INDEX)) {
        while (WinHttpQueryDataAvailable(q, &avail) && avail) {
            char chunk[2048];
            if (!WinHttpReadData(q, chunk, avail < sizeof chunk ? avail : sizeof chunk, &got) ||
                !got)
                break;
            if (have + got < reply_n) {
                memcpy(reply + have, chunk, got);
                have += got;
            }
        }
        *status = (int)code;
        ok = 1;
    } else {
        snprintf(err, err_n, "no answer from %ls (WinHTTP error %lu)", host,
                 (unsigned long)GetLastError());
    }
    reply[have] = 0;
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static int have_transport(void) { return 1; }

#else  /* libcurl, loaded when first needed */

/* The few names used, with the option numbers of curl.h; they have not
 * changed since libcurl 7. */
typedef void CURL;
struct curl_slist;
#define CURLOPT_WRITEDATA       10001
#define CURLOPT_URL             10002
#define CURLOPT_ERRORBUFFER     10010
#define CURLOPT_POSTFIELDS      10015
#define CURLOPT_USERAGENT       10018
#define CURLOPT_HTTPHEADER      10023
#define CURLOPT_WRITEFUNCTION   20011
#define CURLOPT_TIMEOUT         13
#define CURLOPT_POST            47
#define CURLOPT_POSTFIELDSIZE   60
#define CURLOPT_CONNECTTIMEOUT  78
#define CURLOPT_NOSIGNAL        99
#define CURLINFO_RESPONSE_CODE  0x200002
#define CURL_ERROR_SIZE         256

static struct {
    int tried, ok;
    CURL *(*easy_init)(void);
    int (*easy_setopt)(CURL *, int, ...);
    int (*easy_perform)(CURL *);
    int (*easy_getinfo)(CURL *, int, ...);
    void (*easy_cleanup)(CURL *);
    const char *(*easy_strerror)(int);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
} curl;

static int have_transport(void)
{
    static const char *const names[] = {
#ifdef __APPLE__
        "/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib",
#else
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so",
#endif
    };
    void *h = NULL;
    size_t i;

    if (curl.tried)
        return curl.ok;
    curl.tried = 1;
    for (i = 0; i < sizeof names / sizeof names[0] && !h; i++)
        h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return 0;
    *(void **)&curl.easy_init      = dlsym(h, "curl_easy_init");
    *(void **)&curl.easy_setopt    = dlsym(h, "curl_easy_setopt");
    *(void **)&curl.easy_perform   = dlsym(h, "curl_easy_perform");
    *(void **)&curl.easy_getinfo   = dlsym(h, "curl_easy_getinfo");
    *(void **)&curl.easy_cleanup   = dlsym(h, "curl_easy_cleanup");
    *(void **)&curl.easy_strerror  = dlsym(h, "curl_easy_strerror");
    *(void **)&curl.slist_append   = dlsym(h, "curl_slist_append");
    *(void **)&curl.slist_free_all = dlsym(h, "curl_slist_free_all");
    curl.ok = curl.easy_init && curl.easy_setopt && curl.easy_perform &&
              curl.easy_getinfo && curl.easy_cleanup && curl.easy_strerror &&
              curl.slist_append && curl.slist_free_all;
    return curl.ok;
}

typedef struct { char *buf; size_t n, have; } Sink;

static size_t sink_write(char *data, size_t size, size_t count, void *user)
{
    Sink *s = (Sink *)user;
    size_t len = size * count;

    if (s->have + len < s->n) {
        memcpy(s->buf + s->have, data, len);
        s->have += len;
    }
    return len;                         /* past the cap is dropped, not an error */
}

static int post(const char *url, const char *body, int *status, char *reply,
                size_t reply_n, char *err, size_t err_n)
{
    char errbuf[CURL_ERROR_SIZE] = "";
    struct curl_slist *hdr = NULL;
    Sink sink = { reply, reply_n, 0 };
    long code = 0;
    CURL *h;
    int rc;

    h = curl.easy_init();
    if (!h) {
        snprintf(err, err_n, "libcurl would not start");
        return 0;
    }
    hdr = curl.slist_append(hdr, "Content-Type: application/json");
    hdr = curl.slist_append(hdr, "Accept: application/json");
    curl.easy_setopt(h, CURLOPT_URL, url);
    curl.easy_setopt(h, CURLOPT_POST, 1L);
    curl.easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl.easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl.easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    curl.easy_setopt(h, CURLOPT_USERAGENT, user_agent());
    curl.easy_setopt(h, CURLOPT_WRITEFUNCTION, sink_write);
    curl.easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl.easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl.easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    rc = curl.easy_perform(h);
    if (rc == 0)
        curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    else
        snprintf(err, err_n, "%s", errbuf[0] ? errbuf : curl.easy_strerror(rc));
    curl.easy_cleanup(h);
    curl.slist_free_all(hdr);
    reply[sink.have] = 0;
    *status = (int)code;
    return rc == 0;
}

#endif

int insignia_register(const char serial[12], const uint8_t hd_key[16],
                      const uint8_t online_key[16], InsigniaResult *r)
{
    static char reply[REPLY_MAX];
    char body[256], err[300] = "";
    int status = 0, ok;

    memset(r, 0, sizeof *r);
    if (!insignia_register_body(serial, hd_key, online_key, body, sizeof body)) {
        r->outcome = INSIGNIA_BAD_CONSOLE;
        snprintf(r->message, sizeof r->message, "This console cannot be registered: "
                 "its serial is not twelve digits, or its HDD key is zero.");
        return 0;
    }
    if (!have_transport()) {
        r->outcome = INSIGNIA_NO_TRANSPORT;
        snprintf(r->message, sizeof r->message,
                 "Cannot make an HTTPS request here: libcurl was not found.");
        memset(body, 0, sizeof body);
        return 0;
    }
    ok = post(endpoint(), body, &status, reply, sizeof reply, err, sizeof err);
    memset(body, 0, sizeof body);
    if (!ok) {
        r->outcome = INSIGNIA_NO_NETWORK;
        snprintf(r->message, sizeof r->message, "Could not reach Insignia: %s", err);
        return 0;
    }
    insignia_classify(status, reply, r);
    return r->outcome == INSIGNIA_REGISTERED;
}

/* ---- the note in the console folder ----------------------------------- */

int insignia_is_registered(const char *console_dir)
{
    char path[1100];
    FILE *f;

    snprintf(path, sizeof path, "%s" SEP "registered", console_dir);
    f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

int insignia_mark_registered(const char *console_dir)
{
    char path[1100];
    FILE *f;
    int ok;

    snprintf(path, sizeof path, "%s" SEP "registered", console_dir);
    f = fopen(path, "w");
    if (!f)
        return 0;
    ok = fputs("registered with Insignia\n", f) >= 0;
    return fclose(f) == 0 && ok;
}
