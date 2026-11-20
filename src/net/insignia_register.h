/**
 * Registering a console with Insignia: telling its servers the console's
 * serial, HDD key and online key, which they need before the title's own
 * Xbox Live code can sign the console in.
 *
 * Insignia's setup assistant (github.com/insignia-live/setup-assistant-release,
 * an nxdk program) does it with one HTTPS request, and this is that request
 * made from the host, so a recompiled title's console can be registered
 * without running the assistant:
 *
 *     POST https://insignia.live/api/v1/xbox
 *     {"serial_number": "<12 digits>", "hdd_key": "<32 hex>", "online_key": "<32 hex>"}
 *
 * The keys go as upper-case hex, as the assistant formats them. Insignia
 * answers 200 for a console it now knows, 422 with Laravel's validation body
 * ({"message": ..., "errors": {"serial_number": ["The serial number has
 * already been taken."]}}) when it refuses one, and 429 when asked too often.
 *
 * Transport: WinHTTP on Windows; libcurl loaded at run time elsewhere (macOS
 * has it, as do SteamOS and desktop Linux), so nothing is needed to build.
 *
 *     RECOMP_INSIGNIA_URL   the endpoint, for tests against a local server
 *     RECOMP_INSIGNIA_UA    the User-Agent (default DirectXbox-Recomp/<ver>)
 *
 * Nothing here prints a key or the serial.
 */
#ifndef INSIGNIA_REGISTER_H
#define INSIGNIA_REGISTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INSIGNIA_REGISTER_URL "https://insignia.live/api/v1/xbox"

typedef enum {
    INSIGNIA_REGISTERED,    /* 200: Insignia knows the console now */
    INSIGNIA_REFUSED,       /* 422: message says why (serial taken, ...) */
    INSIGNIA_TOO_OFTEN,     /* 429: try again later */
    INSIGNIA_HTTP_ERROR,    /* any other status */
    INSIGNIA_NO_NETWORK,    /* no answer: no connection, DNS, TLS, timeout */
    INSIGNIA_NO_TRANSPORT,  /* nothing here can make an HTTPS request */
    INSIGNIA_BAD_CONSOLE,   /* not sent: serial not 12 digits, or zero HDD key */
} InsigniaOutcome;

typedef struct {
    InsigniaOutcome outcome;
    int  http_status;       /* 0 when there was no answer */
    char message[512];      /* for the player: Insignia's words when it gave some */
} InsigniaResult;

/* The request body. 0 when the console cannot be registered (the serial is
 * not twelve digits, or the HDD key is zero, which Insignia refuses), or out
 * is too small. */
int insignia_register_body(const char serial[12], const uint8_t hd_key[16],
                           const uint8_t online_key[16], char *out, size_t n);

/* What an answer means: the status and its body (NUL-terminated, may be
 * NULL). Fills r->outcome, r->http_status and r->message. */
void insignia_classify(int http_status, const char *body, InsigniaResult *r);

/* Make the request and wait for it (up to 30 s). 1 when Insignia registered
 * the console; r says what happened either way. Blocks: call it off the
 * thread that draws. */
int insignia_register(const char serial[12], const uint8_t hd_key[16],
                      const uint8_t online_key[16], InsigniaResult *r);

/* The console folder's note that this console has been registered, so it is
 * never sent again by itself. */
int insignia_is_registered(const char *console_dir);
int insignia_mark_registered(const char *console_dir);

#ifdef __cplusplus
}
#endif

#endif /* INSIGNIA_REGISTER_H */
