/*
 * launcher_theme_coretext.c -- the macOS half of launcher_theme.h: the back
 * buffer, plain memory, and the text, which CoreText draws straight into it
 * through a bitmap context over the same pixels the shapes are written into
 * (launcher_theme.c).
 *
 * The buffer is the display's own resolution, theme_scale pixels to the
 * window unit, and the context is scaled to match, so text is laid out in
 * window units like everything else and rasterised at full resolution.
 * CoreGraphics counts y upward from the bottom; every rectangle is turned
 * over on its way in rather than the context being flipped, which would
 * turn CoreText's frames upside down with it.
 *
 * The face is the system's own, at the size and weight GDI is asked for:
 * a point size at 96 dots an inch, 400 normal, 600 and up heavier.
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static CGContextRef g_cg;
static double       g_want_scale = 1.0;
static int          g_units_h;          /* the window's height, in its units */

void theme_set_scale(double scale)
{
    g_want_scale = scale > 0.0 ? scale : 1.0;
}

void *theme_image(void)
{
    /* Copy on write: nothing is copied until the buffer is next drawn
     * into, which is a frame from now. */
    return g_cg ? (void *)CGBitmapContextCreateImage(g_cg) : NULL;
}

int theme_begin(int width, int height)
{
    int pw = (int)lround(width * g_want_scale);
    int ph = (int)lround(height * g_want_scale);
    CGColorSpaceRef cs;

    if (g_cg && pw == theme_w && ph == theme_h)
        return 1;
    theme_shutdown();

    theme_px = calloc((size_t)pw * ph, sizeof *theme_px);
    if (!theme_px)
        return 0;
    /* 0x00RRGGBB in a little-endian word is B, G, R, unused in memory:
     * what CoreGraphics calls skip-first, 32-bit little endian. */
    cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    g_cg = CGBitmapContextCreate(theme_px, (size_t)pw, (size_t)ph, 8, (size_t)pw * 4, cs,
                                 (CGBitmapInfo)kCGImageAlphaNoneSkipFirst |
                                     kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!g_cg) {
        free(theme_px);
        theme_px = NULL;
        return 0;
    }
    theme_w = pw;
    theme_h = ph;
    theme_scale = (double)pw / width;
    g_units_h = height;
    CGContextScaleCTM(g_cg, theme_scale, theme_scale);
    CGContextSetAllowsAntialiasing(g_cg, true);
    CGContextSetShouldAntialias(g_cg, true);
    CGContextSetAllowsFontSmoothing(g_cg, true);
    CGContextSetShouldSmoothFonts(g_cg, true);
    return 1;
}

void theme_shutdown(void)
{
    if (g_cg)
        CGContextRelease(g_cg);
    g_cg = NULL;
    free(theme_px);
    theme_px = NULL;
    theme_w = theme_h = 0;
}

/* --------------------------------------------------------------- fonts */

/* A handful of sizes and weights is all the launcher uses; each is made
 * once. */
typedef struct FontSlot { int size, weight; CTFontRef font; } FontSlot;
static FontSlot g_fonts[24];

static CTFontRef font_for(int size, int weight)
{
    CTFontRef base, font;
    int i;

    for (i = 0; i < (int)(sizeof g_fonts / sizeof g_fonts[0]) && g_fonts[i].font; i++)
        if (g_fonts[i].size == size && g_fonts[i].weight == weight)
            return g_fonts[i].font;

    base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size * 96.0 / 72.0, NULL);
    if (!base)
        return NULL;
    font = base;
    if (weight >= 600) {
        /* CoreText's weight runs -1..1 with 0 regular: 0.3 is semibold,
         * 0.4 bold, which is where GDI's 600 and 700 fall. */
        CGFloat w = weight >= 700 ? 0.4 : 0.3;
        CFNumberRef wn = CFNumberCreate(NULL, kCFNumberCGFloatType, &w);
        const void *tk[] = { kCTFontWeightTrait };
        const void *tv[] = { wn };
        CFDictionaryRef traits = CFDictionaryCreate(NULL, tk, tv, 1,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        const void *ak[] = { kCTFontTraitsAttribute };
        const void *av[] = { traits };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef d0 = CTFontCopyFontDescriptor(base);
        CTFontDescriptorRef d1 = CTFontDescriptorCreateCopyWithAttributes(d0, attrs);
        CTFontRef heavy = CTFontCreateWithFontDescriptor(d1, size * 96.0 / 72.0, NULL);

        if (heavy) {
            font = heavy;
            CFRelease(base);
        }
        CFRelease(d1);
        CFRelease(d0);
        CFRelease(attrs);
        CFRelease(traits);
        CFRelease(wn);
    }
    if (i < (int)(sizeof g_fonts / sizeof g_fonts[0])) {
        g_fonts[i].size = size;
        g_fonts[i].weight = weight;
        g_fonts[i].font = font;
    }
    return font;
}

/* The text with its face and colour, or NULL. */
static CFAttributedStringRef styled(const char *text, int size, int weight,
                                    COLORREF colour)
{
    CTFontRef font = font_for(size, weight);
    CFStringRef str;
    CGColorRef cg;
    CFDictionaryRef attrs;
    CFAttributedStringRef out;
    const void *k[2], *v[2];

    if (!font)
        return NULL;
    str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!str)
        str = CFStringCreateWithCString(NULL, text, kCFStringEncodingMacRoman);
    if (!str)
        return NULL;
    cg = CGColorCreateSRGB((colour & 0xFF) / 255.0, ((colour >> 8) & 0xFF) / 255.0,
                           ((colour >> 16) & 0xFF) / 255.0, 1.0);
    k[0] = kCTFontAttributeName;            v[0] = font;
    k[1] = kCTForegroundColorAttributeName; v[1] = cg;
    attrs = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
    out = CFAttributedStringCreate(NULL, str, attrs);
    CFRelease(attrs);
    CGColorRelease(cg);
    CFRelease(str);
    return out;
}

/* A window-unit rectangle, top-left origin, as CoreGraphics counts. */
static CGRect cg_rect(ThemeRect r)
{
    return CGRectMake(r.x, g_units_h - (r.y + r.h), r.w, r.h);
}

/* ---------------------------------------------------------------- text */

void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align)
{
    CFAttributedStringRef s;
    CTLineRef line;
    CGFloat ascent, descent, leading, width, x, baseline;

    if (!g_cg || !text || !*text)
        return;
    s = styled(text, size, weight, colour);
    if (!s)
        return;
    line = CTLineCreateWithAttributedString(s);
    CFRelease(s);
    if (!line)
        return;

    width = (CGFloat)CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    x = r.x;
    if (align == THEME_CENTRE)
        x = r.x + (r.w - width) * 0.5;
    else if (align == THEME_RIGHT)
        x = r.x + r.w - width;
    /* Centred on the line's height, as GDI's DT_VCENTER does. */
    baseline = r.y + (r.h - (ascent + descent)) * 0.5 + ascent;

    CGContextSaveGState(g_cg);
    CGContextClipToRect(g_cg, cg_rect(r));
    CGContextSetTextMatrix(g_cg, CGAffineTransformIdentity);
    CGContextSetTextPosition(g_cg, x, g_units_h - baseline);
    CTLineDraw(line, g_cg);
    CGContextRestoreGState(g_cg);
    CFRelease(line);
}

void theme_text_wrapped(ThemeRect r, const char *text, int size, COLORREF colour)
{
    CFAttributedStringRef s;
    CTFramesetterRef fs;
    CGMutablePathRef path;
    CTFrameRef frame;

    if (!g_cg || !text || !*text)
        return;
    s = styled(text, size, 400, colour);
    if (!s)
        return;
    fs = CTFramesetterCreateWithAttributedString(s);
    CFRelease(s);
    if (!fs)
        return;
    /* A frame lays its lines from the top of its path down, and leaves
     * out a line that does not fit: top aligned and clipped, as GDI's
     * DT_WORDBREAK | DT_TOP is. */
    path = CGPathCreateMutable();
    CGPathAddRect(path, NULL, cg_rect(r));
    frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, NULL);
    if (frame) {
        CGContextSaveGState(g_cg);
        CGContextSetTextMatrix(g_cg, CGAffineTransformIdentity);
        CTFrameDraw(frame, g_cg);
        CGContextRestoreGState(g_cg);
        CFRelease(frame);
    }
    CGPathRelease(path);
    CFRelease(fs);
}
