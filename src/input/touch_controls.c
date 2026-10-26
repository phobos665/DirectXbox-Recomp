/*
 * touch_controls.c - the on-screen controller (touch_controls.h).
 */
#include "touch_controls.h"
#include "recomp_config.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The controls, in Steam Link's default places for a pad with two sticks
 * (CVirtualController's defaults on a 1280x720 screen, as DroidDeck reads
 * them): x and y as fractions of the screen, r in pixels of that screen.
 * The shoulders are the Xbox pad's White and Black. */
enum {
    C_LSTICK, C_RSTICK, C_DPAD, C_A, C_B, C_X, C_Y,
    C_WHITE, C_BLACK, C_LTRIG, C_RTRIG, C_BACK, C_START, C_LTHUMB, C_RTHUMB,
    C_COUNT
};

typedef struct {
    int      kind;
    float    x, y, r;
    unsigned rgb;
    int      analog;            /* bAnalogButtons index, or -1 */
    unsigned bits;              /* wButtons bits */
} control;

static const control k_controls[C_COUNT] = {
    [C_LSTICK] = { TOUCH_SHAPE_STICK,  0.137f, 0.76f, 90, 0xE6E9EC, -1, 0 },
    [C_RSTICK] = { TOUCH_SHAPE_STICK,  0.850f, 0.76f, 90, 0xE6E9EC, -1, 0 },
    [C_DPAD]   = { TOUCH_SHAPE_DPAD,   0.147f, 0.47f, 112, 0xE6E9EC, -1, 0 },
    [C_A]      = { TOUCH_SHAPE_BUTTON, 1083.f / 1280, 607.f / 720 - 0.30f, 38, 0x488530, 0, 0 },
    [C_B]      = { TOUCH_SHAPE_BUTTON, 1163.f / 1280, 527.f / 720 - 0.30f, 38, 0xD34425, 1, 0 },
    [C_X]      = { TOUCH_SHAPE_BUTTON, 1003.f / 1280, 527.f / 720 - 0.30f, 38, 0x173CB8, 2, 0 },
    [C_Y]      = { TOUCH_SHAPE_BUTTON, 1083.f / 1280, 447.f / 720 - 0.30f, 38, 0xEBC846, 3, 0 },
    [C_WHITE]  = { TOUCH_SHAPE_BUTTON, 0.060f, 0.205f, 37, 0xF4F4F4, 5, 0 },
    [C_BLACK]  = { TOUCH_SHAPE_BUTTON, 0.940f, 0.205f, 37, 0x303236, 4, 0 },
    [C_LTRIG]  = { TOUCH_SHAPE_PILL,   0.160f, 0.205f, 37, 0xE6E9EC, 6, 0 },
    [C_RTRIG]  = { TOUCH_SHAPE_PILL,   0.840f, 0.205f, 37, 0xE6E9EC, 7, 0 },
    [C_BACK]   = { TOUCH_SHAPE_PILL,   516.f / 1280, 75.f / 720, 34, 0xE6E9EC, -1, XBOX_GAMEPAD_BACK },
    [C_START]  = { TOUCH_SHAPE_PILL,   756.f / 1280, 75.f / 720, 34, 0xE6E9EC, -1, XBOX_GAMEPAD_START },
    [C_LTHUMB] = { TOUCH_SHAPE_BUTTON, 0.290f, 0.88f, 30, 0xE6E9EC, -1, XBOX_GAMEPAD_LEFT_THUMB },
    [C_RTHUMB] = { TOUCH_SHAPE_BUTTON, 0.710f, 0.88f, 30, 0xE6E9EC, -1, XBOX_GAMEPAD_RIGHT_THUMB },
};

/* A pill's half-extents, as multiples of r: wider than tall. */
#define PILL_HALF_W 1.25f
#define PILL_HALF_H 0.75f

#define MAX_FINGERS   10
#define FADE_AFTER_MS 4000
#define FADED         0.3f

typedef struct {
    SDL_FingerID id;
    int          control;       /* C_*, or -1 for a touch on nothing */
    float        x, y;          /* pixels */
} finger;

enum { MODE_OFF, MODE_AUTO, MODE_ON };

static struct {
    SDL_Mutex *lock;
    int        mode;
    int        started;
    int        shown;           /* auto: shown now */
    float      opacity, scale;
    int        w, h;            /* the screen the shapes were last asked for */
    finger     fingers[MAX_FINGERS];
    int        nfingers;
    Uint64     last_touch_ms;
} T = { NULL, MODE_OFF, 0, 0, 0.45f, 1.0f, 1920, 1080 };

static float unit(void)
{
    float u = (float)T.w / 1280.0f, v = (float)T.h / 720.0f;
    return (u < v ? u : v) * T.scale;
}

static float cx(int c) { return k_controls[c].x * (float)T.w; }
static float cy(int c) { return k_controls[c].y * (float)T.h; }
static float radius(int c) { return k_controls[c].r * unit(); }

/* How far (x, y) is from a control's centre, in multiples of its size: 1 is
 * its edge. A pill measures against its own box. */
static float reach(int c, float x, float y)
{
    float dx = x - cx(c), dy = y - cy(c), r = radius(c);

    if (k_controls[c].kind == TOUCH_SHAPE_PILL) {
        float ax = fabsf(dx) / (r * PILL_HALF_W), ay = fabsf(dy) / (r * PILL_HALF_H);
        return ax > ay ? ax : ay;
    }
    return sqrtf(dx * dx + dy * dy) / r;
}

/* The control under (x, y), the nearest when two overlap; -1 for none. */
static int hit(float x, float y, int buttons_only)
{
    int c, best = -1;
    float best_reach = 1.0f;

    for (c = 0; c < C_COUNT; c++) {
        float d;
        if (buttons_only && (k_controls[c].kind == TOUCH_SHAPE_STICK ||
                             k_controls[c].kind == TOUCH_SHAPE_DPAD))
            continue;
        d = reach(c, x, y);
        if (d <= best_reach) {
            best_reach = d;
            best = c;
        }
    }
    return best;
}

static int is_button(int c)
{
    return c >= 0 && k_controls[c].kind != TOUCH_SHAPE_STICK &&
           k_controls[c].kind != TOUCH_SHAPE_DPAD;
}

static finger *find_finger(SDL_FingerID id)
{
    int i;
    for (i = 0; i < T.nfingers; i++)
        if (T.fingers[i].id == id)
            return &T.fingers[i];
    return NULL;
}

static bool SDLCALL on_event(void *user, SDL_Event *e)
{
    float x, y;
    finger *f;

    (void)user;
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED)
        return true;
    SDL_LockMutex(T.lock);
    x = e->tfinger.x * (float)T.w;
    y = e->tfinger.y * (float)T.h;
    T.last_touch_ms = SDL_GetTicks();
    f = find_finger(e->tfinger.fingerID);
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        if (!T.shown) {
            /* The first touch only brings the controls back. */
            T.shown = 1;
            break;
        }
        if (!f && T.nfingers < MAX_FINGERS) {
            f = &T.fingers[T.nfingers++];
            f->id = e->tfinger.fingerID;
            f->control = hit(x, y, 0);
            f->x = x;
            f->y = y;
        }
        break;
    case SDL_EVENT_FINGER_MOTION:
        if (f) {
            f->x = x;
            f->y = y;
            /* A finger on a button can slide onto another. */
            if (is_button(f->control) && reach(f->control, x, y) > 1.0f) {
                int over = hit(x, y, 1);
                if (over >= 0)
                    f->control = over;
            }
        }
        break;
    default:                            /* up, cancelled */
        if (e->type == SDL_EVENT_FINGER_CANCELED) {
            T.nfingers = 0;
        } else if (f) {
            *f = T.fingers[--T.nfingers];
        }
        break;
    }
    SDL_UnlockMutex(T.lock);
    return true;
}

static float setting_float(const char *env, const char *key, float def, float lo, float hi)
{
    const char *v = recomp_config_lookup(env, key);
    float f;

    if (!v || !*v)
        return def;
    f = (float)atof(v);
    return f < lo ? lo : f > hi ? hi : f;
}

int recomp_touch_enabled(void)
{
    recomp_touch_init();
    return T.mode != MODE_OFF;
}

void recomp_touch_init(void)
{
    const char *v;

    if (T.started)
        return;
    T.started = 1;
    v = recomp_config_lookup("RECOMP_TOUCH_CONTROLS", "touch_controls");
#if defined(__ANDROID__)
    T.mode = MODE_AUTO;
#else
    T.mode = MODE_OFF;
#endif
    if (v && *v) {
        if (!strcmp(v, "on") || !strcmp(v, "1"))
            T.mode = MODE_ON;
        else if (!strcmp(v, "off") || !strcmp(v, "0"))
            T.mode = MODE_OFF;
        else if (!strcmp(v, "auto"))
            T.mode = MODE_AUTO;
    }
    if (T.mode == MODE_OFF)
        return;
    T.opacity = setting_float("RECOMP_TOUCH_OPACITY", "touch_opacity", 0.45f, 0.1f, 1.0f);
    T.scale = setting_float("RECOMP_TOUCH_SCALE", "touch_scale", 1.0f, 0.5f, 2.0f);
    T.lock = SDL_CreateMutex();
    if (!T.lock) {
        T.mode = MODE_OFF;
        return;
    }
    {
        int n = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&n);
        SDL_free(pads);
        /* auto: shown from the start only with no pad to play with. */
        T.shown = T.mode == MODE_ON || n == 0;
    }
    T.last_touch_ms = SDL_GetTicks();
    SDL_AddEventWatch(on_event, NULL);
    fprintf(stderr, "[INPUT] touch controls %s (%s), opacity %.2f, scale %.2f\n",
            T.mode == MODE_ON ? "on" : "auto", T.shown ? "shown" : "hidden until a touch",
            T.opacity, T.scale);
}

/* What one stick finger says, as a deflection of -32767..32767 a side. */
static void stick_value(int c, const finger *f, SHORT *sx, SHORT *sy)
{
    float r = radius(c);
    float dx = (f->x - cx(c)) / r, dy = (f->y - cy(c)) / r;
    float d = sqrtf(dx * dx + dy * dy);

    if (d > 1.0f) {
        dx /= d;
        dy /= d;
    }
    *sx = (SHORT)(dx * 32767.0f);
    *sy = (SHORT)(-dy * 32767.0f);      /* the pad's y is up */
}

/* The d-pad's directions under one finger: eight sectors, each direction
 * covering 67.5 degrees so a diagonal presses two; nothing in the middle. */
static unsigned dpad_bits(const finger *f)
{
    float dx = f->x - cx(C_DPAD), dy = f->y - cy(C_DPAD);
    float a;
    unsigned bits = 0;

    if (sqrtf(dx * dx + dy * dy) < radius(C_DPAD) * 0.22f)
        return 0;
    a = atan2f(-dy, dx) * (180.0f / 3.14159265f);
    if (a < 0)
        a += 360.0f;
    if (a < 67.5f || a > 292.5f)   bits |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (a >= 22.5f && a <= 157.5f)  bits |= XBOX_GAMEPAD_DPAD_UP;
    if (a >= 112.5f && a <= 247.5f) bits |= XBOX_GAMEPAD_DPAD_LEFT;
    if (a >= 202.5f && a <= 337.5f) bits |= XBOX_GAMEPAD_DPAD_DOWN;
    return bits;
}

static int pad_pressed(const XBOX_GAMEPAD *p)
{
    int i;
    if (p->wButtons)
        return 1;
    for (i = 0; i < 8; i++)
        if (p->bAnalogButtons[i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            return 1;
    return abs(p->sThumbLX) > 12000 || abs(p->sThumbLY) > 12000 ||
           abs(p->sThumbRX) > 12000 || abs(p->sThumbRY) > 12000;
}

static void take_larger(SHORT *dst, SHORT v)
{
    if (abs(v) > abs(*dst))
        *dst = v;
}

void recomp_touch_apply(XBOX_GAMEPAD *pad)
{
    int i;

    recomp_touch_init();
    if (T.mode == MODE_OFF || !pad)
        return;
    SDL_LockMutex(T.lock);
    if (T.mode == MODE_AUTO && T.shown && T.nfingers == 0 && pad_pressed(pad)) {
        T.shown = 0;                    /* a real pad is being played */
        fprintf(stderr, "[INPUT] touch controls hidden: a pad is in use (a touch brings them back)\n");
    }
    for (i = 0; i < T.nfingers && T.shown; i++) {
        const finger *f = &T.fingers[i];
        const control *c;
        SHORT sx, sy;

        if (f->control < 0)
            continue;
        c = &k_controls[f->control];
        switch (c->kind) {
        case TOUCH_SHAPE_STICK:
            stick_value(f->control, f, &sx, &sy);
            if (f->control == C_LSTICK) {
                take_larger(&pad->sThumbLX, sx);
                take_larger(&pad->sThumbLY, sy);
            } else {
                take_larger(&pad->sThumbRX, sx);
                take_larger(&pad->sThumbRY, sy);
            }
            break;
        case TOUCH_SHAPE_DPAD:
            pad->wButtons |= (WORD)dpad_bits(f);
            break;
        default:
            pad->wButtons |= (WORD)c->bits;
            if (c->analog >= 0)
                pad->bAnalogButtons[c->analog] = 255;
            break;
        }
    }
    SDL_UnlockMutex(T.lock);
}

int recomp_touch_shapes(TouchShape *out, int max, int w, int h, float *opacity)
{
    int c, n = 0, i;
    Uint64 idle;

    if (T.mode == MODE_OFF || !T.lock || w <= 0 || h <= 0)
        return 0;
    SDL_LockMutex(T.lock);
    T.w = w;
    T.h = h;
    if (!T.shown) {
        SDL_UnlockMutex(T.lock);
        return 0;
    }
    idle = SDL_GetTicks() - T.last_touch_ms;
    if (opacity)
        *opacity = T.opacity * (T.nfingers == 0 && idle > FADE_AFTER_MS ? FADED : 1.0f);
    for (c = 0; c < C_COUNT && n < max; c++) {
        TouchShape *s = &out[n++];
        const control *k = &k_controls[c];

        memset(s, 0, sizeof *s);
        s->kind = k->kind;
        s->x = cx(c);
        s->y = cy(c);
        s->r = radius(c);
        s->rgb = k->rgb;
        if (k->kind == TOUCH_SHAPE_PILL) {
            s->w = 2.0f * PILL_HALF_W * s->r;
            s->h = 2.0f * PILL_HALF_H * s->r;
            s->r = PILL_HALF_H * s->r;
        }
        for (i = 0; i < T.nfingers; i++) {
            const finger *f = &T.fingers[i];
            if (f->control != c)
                continue;
            if (k->kind == TOUCH_SHAPE_STICK) {
                SHORT sx, sy;
                stick_value(c, f, &sx, &sy);
                s->kx = (float)sx / 32767.0f * s->r;
                s->ky = -(float)sy / 32767.0f * s->r;
                s->pressed = 1;
            } else if (k->kind == TOUCH_SHAPE_DPAD) {
                s->pressed |= dpad_bits(f);
            } else {
                s->pressed = 1;
            }
        }
    }
    SDL_UnlockMutex(T.lock);
    return n;
}
