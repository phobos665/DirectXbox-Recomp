/*
 * touch_controls.h - an on-screen controller, for a device with no pad.
 *
 * A layout of sticks and buttons over the screen, driven by SDL's finger
 * events and added to controller 1 on top of its bindings
 * (recomp_bindings_sample). The renderer draws the same layout over the
 * finished frame (src/d3d/d3d8_touch.c), handed it by src/hle; nothing here
 * draws.
 *
 * The layout and its behaviour follow Steam Link's virtual controller, as
 * DroidDeck uses it: positions as fractions of the screen, sizes in pixels of
 * a 1280x720 screen scaled by min(w / 1280, h / 720), a stick read as its
 * offset clamped to the ring, a d-pad of eight 67.5 degree sectors, and a
 * finger that slides from one button onto another.
 *
 *   touch_controls = auto | on | off                    [RECOMP_TOUCH_CONTROLS]
 *   touch_opacity  = 0.1 .. 1.0 (default 0.45)          [RECOMP_TOUCH_OPACITY]
 *   touch_scale    = 0.5 .. 2.0 (default 1.0)           [RECOMP_TOUCH_SCALE]
 *
 * auto, the default on Android and off elsewhere: shown while no pad is
 * connected, shown again by a touch, and hidden by a press on a real pad.
 * Untouched for a few seconds, the controls fade.
 */
#ifndef XBOXRECOMP_TOUCH_CONTROLS_H
#define XBOXRECOMP_TOUCH_CONTROLS_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TouchShape, the controls as drawn: shared with the renderer. */
#include "platform/touch_shape.h"

/* Starts listening for fingers (an SDL event watch). Safe to call more than
 * once; does nothing when the setting is off. */
void recomp_touch_init(void);

/* Whether the controls are switched on at all (not off). */
int  recomp_touch_enabled(void);

/* Adds what the fingers hold now into *pad: buttons OR'd, analog buttons and
 * stick deflections taken where they are larger. A pad whose own sample
 * (before this) presses anything hides the controls in auto mode. */
void recomp_touch_apply(XBOX_GAMEPAD *pad);

/* The shapes to draw now on a screen of w x h pixels, and the opacity to
 * draw them at (already faded). Returns how many were written, 0 when the
 * controls are hidden. Any thread. */
int  recomp_touch_shapes(TouchShape *out, int max, int w, int h, float *opacity);

#ifdef __cplusplus
}
#endif

#endif
