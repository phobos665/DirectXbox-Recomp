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

/* The controls. Where they go is DroidDeck's GamepadGeometry, its layout
 * for a screen-wide pad on a wide screen: each thumb pivots at a bottom
 * corner, its stick diagonally in from the corner and the d-pad (left) or
 * the face buttons (right) a slide inward along the arc, two shoulders above
 * on each side -- the triggers outside, White and Black inside -- and Back,
 * the stick clicks and Start between the arcs. What each control looks like
 * and does follows Steam Link's controller, as DroidDeck draws it. */
enum {
    C_LSTICK, C_RSTICK, C_DPAD, C_A, C_B, C_X, C_Y,
    C_WHITE, C_BLACK, C_LTRIG, C_RTRIG, C_BACK, C_START, C_LTHUMB, C_RTHUMB,
    C_COUNT
};

typedef struct {
    int      kind;
    unsigned rgb;
    int      analog;            /* bAnalogButtons index, or -1 */
    unsigned bits;              /* wButtons bits */
} control;

static const control k_controls[C_COUNT] = {
    [C_LSTICK] = { TOUCH_SHAPE_STICK,  0xE6E9EC, -1, 0 },
    [C_RSTICK] = { TOUCH_SHAPE_STICK,  0xE6E9EC, -1, 0 },
    [C_DPAD]   = { TOUCH_SHAPE_DPAD,   0xE6E9EC, -1, 0 },
    [C_A]      = { TOUCH_SHAPE_BUTTON, 0x488530, 0, 0 },
    [C_B]      = { TOUCH_SHAPE_BUTTON, 0xD34425, 1, 0 },
    [C_X]      = { TOUCH_SHAPE_BUTTON, 0x173CB8, 2, 0 },
    [C_Y]      = { TOUCH_SHAPE_BUTTON, 0xEBC846, 3, 0 },
    [C_WHITE]  = { TOUCH_SHAPE_PILL,   0xF4F4F4, 5, 0 },
    [C_BLACK]  = { TOUCH_SHAPE_PILL,   0x6A6D72, 4, 0 },
    [C_LTRIG]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, 6, 0 },
    [C_RTRIG]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, 7, 0 },
    [C_BACK]   = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_BACK },
    [C_START]  = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_START },
    [C_LTHUMB] = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_LEFT_THUMB },
    [C_RTHUMB] = { TOUCH_SHAPE_PILL,   0xE6E9EC, -1, XBOX_GAMEPAD_RIGHT_THUMB },
};

/* Where each control is on the current screen, in pixels: a centre and a
 * box (a disc's box is its diameter both ways). */
typedef struct {
    float x, y, w, h;
} place;

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
    float      density;         /* pixels per dp */
    int        w, h;            /* the screen the shapes were last asked for */
    int        laid_w, laid_h;  /* the screen `places` is for */
    place      places[C_COUNT];
    finger     fingers[MAX_FINGERS];
    int        nfingers;
    Uint64     last_touch_ms;
} T = { NULL, MODE_OFF, 0, 0, 0.45f, 1.0f, 0.0f, 1920, 1080 };

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

static void put(int c, float x, float y, float w, float h)
{
    T.places[c].x = x;
    T.places[c].y = y;
    T.places[c].w = w;
    T.places[c].h = h;
}

/* GamepadGeometry.layout's wide branch, for a w x h screen. */
static void lay_out(void)
{
    float w = (float)T.w, h = (float)T.h;
    float d = T.density > 0.0f ? T.density : h / 411.0f;
    float pad = 8 * d, button_h = 40 * d, row_button = 64 * d, grid_button = 48 * d;
    float gutter = 8 * d;
    float reach = minf(h - pad, w * 0.33f) * T.scale;
    float main = reach * 0.66f, second = reach * 0.54f;
    float main_top = h - reach * 0.40f - main / 2;
    float shoulder_w = reach * 0.36f, shoulder_h = maxf(reach * 0.16f, button_h);
    float shoulder_y = maxf(minf(h - reach * 0.94f + shoulder_h / 2,
                                 main_top - gutter - shoulder_h / 2), pad + shoulder_h / 2);
    float face_x = w - reach * 1.03f, face_y = h - reach * 0.30f;
    float face_r = second * 0.19f, face_off = second * 0.30f;
    float gap = w - 2 * reach * 1.32f, span, each;
    int columns, i;
    /* The middle row, in DroidDeck's order (View, Steam, QAM, Menu), and its
     * two-by-two places (View and Menu above Steam and QAM). */
    static const int middle[4] = { C_BACK, C_LTHUMB, C_RTHUMB, C_START };
    static const int grid[4] = { 0, 2, 3, 1 };

    put(C_LSTICK, reach * 0.40f, h - reach * 0.40f, main, main);
    put(C_RSTICK, w - reach * 0.40f, h - reach * 0.40f, main, main);
    put(C_DPAD, reach * 1.03f, h - reach * 0.30f, second, second);
    put(C_A, face_x, face_y + face_off, 2 * face_r, 2 * face_r);
    put(C_B, face_x + face_off, face_y, 2 * face_r, 2 * face_r);
    put(C_X, face_x - face_off, face_y, 2 * face_r, 2 * face_r);
    put(C_Y, face_x, face_y - face_off, 2 * face_r, 2 * face_r);
    put(C_LTRIG, reach * 0.22f, shoulder_y, shoulder_w, shoulder_h);
    put(C_RTRIG, w - reach * 0.22f, shoulder_y, shoulder_w, shoulder_h);
    put(C_WHITE, reach * 0.62f, shoulder_y, shoulder_w, shoulder_h);
    put(C_BLACK, w - reach * 0.62f, shoulder_y, shoulder_w, shoulder_h);

    /* Between the arcs: in a row if they fit, else two by two, else one
     * above another. */
    columns = gap >= 4 * row_button ? 4 : gap >= 2 * grid_button ? 2 : 1;
    span = gap - 2 * gutter;
    each = minf(span / (float)columns, 110 * d);
    for (i = 0; i < 4; i++) {
        int index = columns == 2 ? grid[i] : i;
        float x = w / 2 + ((float)(index % columns) - (float)(columns - 1) / 2) * each;
        float top = shoulder_y - button_h / 2;
        put(middle[i], x, top + button_h / 2 + (float)(index / columns) * (button_h + 10 * d),
            each - gutter, button_h);
    }
    T.laid_w = T.w;
    T.laid_h = T.h;
}

static const place *at(int c)
{
    if (T.laid_w != T.w || T.laid_h != T.h)
        lay_out();
    return &T.places[c];
}

static float cx(int c) { return at(c)->x; }
static float cy(int c) { return at(c)->y; }
static float radius(int c) { return at(c)->w * 0.5f; }

/* How far (x, y) is from a control's centre, in multiples of its size: 1 is
 * its edge. A pill measures against its own box. */
static float reach(int c, float x, float y)
{
    const place *p = at(c);
    float dx = x - p->x, dy = y - p->y;

    if (k_controls[c].kind == TOUCH_SHAPE_PILL) {
        float ax = fabsf(dx) / (p->w * 0.5f), ay = fabsf(dy) / (p->h * 0.5f);
        return ax > ay ? ax : ay;
    }
    return sqrtf(dx * dx + dy * dy) / (p->w * 0.5f);
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
    /* auto: a pad that arrives after start-up (enumeration is not done
     * when this starts) hides the controls as a press on it would. */
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && T.mode == MODE_AUTO) {
        SDL_LockMutex(T.lock);
        if (T.shown && T.nfingers == 0) {
            T.shown = 0;
            fprintf(stderr, "[INPUT] touch controls hidden: a pad is connected (a touch brings them back)\n");
        }
        SDL_UnlockMutex(T.lock);
        return true;
    }
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
    {
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        /* Android's density (a dp in pixels); elsewhere the desktop scale. */
        T.density = scale > 0.0f ? scale : 0.0f;
    }
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
            /* Rounded squares, as DroidDeck draws its shoulders. */
            s->w = at(c)->w;
            s->h = at(c)->h;
            s->r = minf(s->w, s->h) * 0.22f;
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
