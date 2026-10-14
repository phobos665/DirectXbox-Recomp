/*
 * launcher_cocoa.m -- a frame on screen, for launcher_sdl.c on macOS.
 *
 * The toolkit's SDL is built without its renderer (nothing in the runtime
 * draws through it), and with no renderer SDL has no way to put a buffer
 * in a Cocoa window either. Core Animation does it directly: the theme's
 * buffer, as an image, is the contents of the window's content view's
 * layer, scaled to fill it -- which at the window's own pixel density is
 * no scaling at all.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

void launcher_cocoa_present(void *nswindow, void *cgimage)
{
    NSWindow *window = (__bridge NSWindow *)nswindow;
    NSView *view = [window contentView];

    if (!view || !cgimage)
        return;
    if (![view wantsLayer])
        [view setWantsLayer:YES];
    /* No implicit animation: each frame replaces the last outright. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [view layer].contentsGravity = kCAGravityResize;
    [view layer].contents = (__bridge id)cgimage;
    [CATransaction commit];
}
