/*
 * launcher_theme_gdi.c -- the Windows half of launcher_theme.h: the back
 * buffer, a 32-bit DIB, and the text, which goes through GDI. GDI can draw
 * into the same DIB the shapes are written into (launcher_theme.c) and
 * already knows how to shape and hint a font. The launcher asks Windows not
 * to scale it, so a window unit is a pixel here.
 */
#include "launcher_theme.h"
#include "launcher_theme_buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static HDC      g_dc;
static HBITMAP  g_bmp, g_old;

int theme_begin(int width, int height)
{
    BITMAPINFO bi;

    if (g_dc && width == theme_w && height == theme_h)
        return 1;
    theme_shutdown();

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_dc = CreateCompatibleDC(NULL);
    if (!g_dc)
        return 0;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&theme_px, NULL, 0);
    if (!g_bmp) {
        DeleteDC(g_dc);
        g_dc = NULL;
        return 0;
    }
    g_old = (HBITMAP)SelectObject(g_dc, g_bmp);
    theme_w = width;
    theme_h = height;
    theme_scale = 1.0;
    SetBkMode(g_dc, TRANSPARENT);
    return 1;
}

void theme_shutdown(void)
{
    if (g_dc) {
        SelectObject(g_dc, g_old);
        DeleteObject(g_bmp);
        DeleteDC(g_dc);
    }
    g_dc = NULL;
    g_bmp = NULL;
    theme_px = NULL;
    theme_w = theme_h = 0;
}

void theme_present(HDC dc)
{
    if (g_dc)
        BitBlt(dc, 0, 0, theme_w, theme_h, g_dc, 0, 0, SRCCOPY);
}

/* --------------------------------------------------------------- text */

void theme_text(ThemeRect r, const char *text, int size, int weight,
                COLORREF colour, ThemeAlign align)
{
    HFONT font, old;
    RECT rc;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;

    if (!g_dc || !text)
        return;

    /* A face that is on every Windows install, and is not the console's.
     * Segoe UI where it exists, Arial where it does not. */
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!font)
        return;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextColor(g_dc, colour);

    rc.left = r.x;
    rc.top = r.y;
    rc.right = r.x + r.w;
    rc.bottom = r.y + r.h;
    if (align == THEME_CENTRE) flags |= DT_CENTER;
    else if (align == THEME_RIGHT) flags |= DT_RIGHT;

    DrawTextA(g_dc, text, -1, &rc, flags);
    SelectObject(g_dc, old);
    DeleteObject(font);
}

void theme_text_wrapped(ThemeRect r, const char *text, int size, COLORREF colour)
{
    HFONT font, old;
    RECT rc;

    if (!g_dc || !text)
        return;
    font = CreateFontA(-MulDiv(size, 96, 72), 0, 0, 0, 400, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    if (!font)
        return;
    old = (HFONT)SelectObject(g_dc, font);
    SetTextColor(g_dc, colour);

    rc.left = r.x;
    rc.top = r.y;
    rc.right = r.x + r.w;
    rc.bottom = r.y + r.h;
    DrawTextA(g_dc, text, -1, &rc, DT_WORDBREAK | DT_TOP | DT_NOPREFIX);
    SelectObject(g_dc, old);
    DeleteObject(font);
}
