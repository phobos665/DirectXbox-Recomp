/*
 * launcher_iso_win32.c -- the game's files from the player's own disc image.
 *
 * A port is shared without the game: the executable carries the runtime and
 * the code lifted from one known XBE, and the files that make the game --
 * that XBE included -- come from the disc the player owns. So the first time
 * the launcher finds no game beside itself, it asks for the image (a full
 * Redump .iso or a trimmed xiso), copies the XDVDFS file system out of it
 * into game\ beside the executable, and checks the XBE against the one the
 * code was lifted from (LAUNCHER_XBE_SHA1, recomp_add_launcher XBE_SHA1).
 *
 * XDVDFS: the volume descriptor is sector 32 of the game partition, which
 * starts at 0 in an xiso and further in for a full image (XGD1 0x18300000;
 * XGD2/XGD3 offsets are tried too). A directory is a binary tree of entries,
 * each { u16 left, u16 right (in dwords from the table's start), u32 sector,
 * u32 size, u8 attributes, u8 name length, name }; 0xFFFF fills the rest of
 * a sector.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "launcher.h"

#ifndef LAUNCHER_GAME_XBE
#define LAUNCHER_GAME_XBE "default.xbe"
#endif

#define XDVDFS_SECTOR 2048ull
#define ISO_BUF (4u << 20)

static const uint64_t k_partitions[] = {
    0ull, 0x18300000ull, 0x2080000ull, 0xFD90000ull
};

typedef struct {
    HANDLE   iso;
    uint64_t base;                 /* the game partition's offset */
    uint64_t total, done;          /* bytes, for the progress line */
    unsigned files;
    int      failed;
    wchar_t  error[512];
    uint8_t *buf;
} IsoJob;

static volatile LONG g_iso_progress;    /* tenths of a percent */
static volatile LONG g_iso_finished;
static IsoJob g_job;
static wchar_t g_iso_path[MAX_PATH];
static wchar_t g_game_dir[MAX_PATH];

static int read_at(HANDLE h, uint64_t off, void *dst, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;

    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return 0;
    return ReadFile(h, dst, n, &got, NULL) && got == n;
}

static int find_partition(HANDLE h, uint64_t *base, uint32_t *root_sector, uint32_t *root_size)
{
    size_t i;
    uint8_t vd[0x800];

    for (i = 0; i < sizeof k_partitions / sizeof k_partitions[0]; i++) {
        uint64_t b = k_partitions[i];
        if (!read_at(h, b + 32 * XDVDFS_SECTOR, vd, sizeof vd))
            continue;
        if (memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0 ||
            memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20) != 0)
            continue;
        *base = b;
        memcpy(root_sector, vd + 20, 4);
        memcpy(root_size, vd + 24, 4);
        return 1;
    }
    return 0;
}

static void job_fail(IsoJob *j, const wchar_t *what, const wchar_t *path)
{
    if (j->failed)
        return;
    j->failed = 1;
    _snwprintf(j->error, sizeof j->error / sizeof j->error[0] - 1, L"%s\n%s",
               what, path ? path : L"");
}

static int copy_file(IsoJob *j, uint32_t sector, uint32_t size, const wchar_t *dst)
{
    HANDLE out;
    uint64_t off = j->base + (uint64_t)sector * XDVDFS_SECTOR;
    uint32_t left = size;

    out = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        job_fail(j, L"Could not write / Konnte nicht schreiben:", dst);
        return 0;
    }
    while (left) {
        DWORD n = left > ISO_BUF ? ISO_BUF : left, wrote = 0;
        if (!read_at(j->iso, off, j->buf, n)) {
            job_fail(j, L"The image could not be read / Das Image ist nicht lesbar:", g_iso_path);
            CloseHandle(out);
            return 0;
        }
        if (!WriteFile(out, j->buf, n, &wrote, NULL) || wrote != n) {
            job_fail(j, L"Could not write (disk full?) / Schreiben fehlgeschlagen (Platte voll?):", dst);
            CloseHandle(out);
            return 0;
        }
        off += n;
        left -= n;
        j->done += n;
        if (j->total)
            InterlockedExchange(&g_iso_progress, (LONG)(j->done * 1000ull / j->total));
    }
    CloseHandle(out);
    j->files++;
    return 1;
}

/* Walk one directory table; with dst NULL only the sizes are added up. */
static int walk_dir(IsoJob *j, uint32_t sector, uint32_t size, const wchar_t *dst, int depth)
{
    uint8_t *table;
    uint32_t *stack;
    uint32_t cap;
    int sp = 0;

    if (depth > 32 || size == 0)
        return 1;
    if (size > (64u << 20)) {
        job_fail(j, L"The image looks damaged / Das Image scheint beschaedigt:", g_iso_path);
        return 0;
    }
    table = (uint8_t *)malloc(size);
    if (!table || !read_at(j->iso, j->base + (uint64_t)sector * XDVDFS_SECTOR, table, size)) {
        free(table);
        job_fail(j, L"The image could not be read / Das Image ist nicht lesbar:", g_iso_path);
        return 0;
    }
    /* Every entry is at least 14 bytes, so no more can be pending. */
    cap = size / 14u + 2u;
    stack = (uint32_t *)malloc(cap * sizeof *stack);
    if (!stack) {
        free(table);
        job_fail(j, L"Out of memory / Kein Speicher frei", NULL);
        return 0;
    }
    stack[sp++] = 0;
    while (sp > 0 && !j->failed) {
        uint32_t off = stack[--sp];
        uint16_t lft, rgt;
        uint32_t esec, esize;
        uint8_t attr, nlen;
        char name[256];
        wchar_t wname[256], path[MAX_PATH];

        if (off + 14 > size)
            continue;
        memcpy(&lft, table + off, 2);
        memcpy(&rgt, table + off + 2, 2);
        if (lft == 0xFFFF && rgt == 0xFFFF)
            continue;
        memcpy(&esec, table + off + 4, 4);
        memcpy(&esize, table + off + 8, 4);
        attr = table[off + 12];
        nlen = table[off + 13];
        if (off + 14 + nlen > size)
            continue;
        if (lft && (uint32_t)sp < cap)
            stack[sp++] = (uint32_t)lft * 4u;
        if (rgt && (uint32_t)sp < cap)
            stack[sp++] = (uint32_t)rgt * 4u;
        memcpy(name, table + off + 14, nlen);
        name[nlen] = 0;
        if (!nlen || strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
            continue;
        if (!MultiByteToWideChar(1252, 0, name, -1, wname, 256))
            continue;
        if (attr & 0x10) {
            if (dst) {
                _snwprintf(path, MAX_PATH - 1, L"%s\\%s", dst, wname);
                path[MAX_PATH - 1] = 0;
                CreateDirectoryW(path, NULL);
                walk_dir(j, esec, esize, path, depth + 1);
            } else {
                walk_dir(j, esec, esize, NULL, depth + 1);
            }
        } else if (dst) {
            _snwprintf(path, MAX_PATH - 1, L"%s\\%s", dst, wname);
            path[MAX_PATH - 1] = 0;
            copy_file(j, esec, esize, path);
        } else {
            j->total += esize;
        }
    }
    free(stack);
    free(table);
    return !j->failed;
}

static DWORD WINAPI extract_thread(LPVOID unused)
{
    IsoJob *j = &g_job;
    uint32_t root_sector = 0, root_size = 0;

    (void)unused;
    j->iso = CreateFileW(g_iso_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (j->iso == INVALID_HANDLE_VALUE) {
        job_fail(j, L"The image could not be opened / Das Image laesst sich nicht oeffnen:", g_iso_path);
    } else if (!find_partition(j->iso, &j->base, &root_sector, &root_size)) {
        job_fail(j, L"This is not an Xbox disc image / Das ist kein Xbox-Disc-Image:", g_iso_path);
    } else if ((j->buf = (uint8_t *)malloc(ISO_BUF)) == NULL) {
        job_fail(j, L"Out of memory / Kein Speicher frei", NULL);
    } else {
        CreateDirectoryW(g_game_dir, NULL);
        if (walk_dir(j, root_sector, root_size, NULL, 0))
            walk_dir(j, root_sector, root_size, g_game_dir, 0);
    }
    if (j->iso != INVALID_HANDLE_VALUE)
        CloseHandle(j->iso);
    free(j->buf);
    InterlockedExchange(&g_iso_finished, 1);
    return 0;
}

#ifdef LAUNCHER_XBE_SHA1
/* SHA-1 of a file, as 40 lower-case hex digits. 0 when it cannot be read. */
static int sha1_file(const wchar_t *path, char hex[41])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE f;
    uint8_t buf[65536], digest[20];
    DWORD got;
    int ok = 0, i;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) {
            if (BCryptHashData(hash, buf, got, 0) != 0) {
                ok = 0;
                break;
            }
        }
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) != 0)
            ok = 0;
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (ok)
        for (i = 0; i < 20; i++)
            sprintf(hex + 2 * i, "%02x", digest[i]);
    return ok;
}
#endif

/* ------------------------------------------------------------ the window */

static HWND g_text;

static LRESULT CALLBACK progress_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER) {
        wchar_t line[256];
        LONG p = InterlockedCompareExchange(&g_iso_progress, 0, 0);

        _snwprintf(line, 255,
                   L"Copying the game from your disc image...\r\n"
                   L"Das Spiel wird aus deinem Disc-Image kopiert...\r\n\r\n%ld.%ld %%",
                   p / 10, p % 10);
        line[255] = 0;
        SetWindowTextW(g_text, line);
        if (InterlockedCompareExchange(&g_iso_finished, 0, 0))
            DestroyWindow(h);
        return 0;
    }
    if (msg == WM_CLOSE)
        return 0;                           /* not while copying */
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void run_progress_window(HINSTANCE inst)
{
    WNDCLASSW wc;
    HWND w;
    MSG m;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_WAIT);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"xboxrecomp_iso_import";
    RegisterClassW(&wc);
    w = CreateWindowW(wc.lpszClassName, L"Disc image / Disc-Image",
                      WS_OVERLAPPED | WS_CAPTION, CW_USEDEFAULT, CW_USEDEFAULT, 460, 170,
                      NULL, NULL, inst, NULL);
    g_text = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
                           10, 20, 420, 90, w, NULL, inst, NULL);
    ShowWindow(w, SW_SHOW);
    SetTimer(w, 1, 100, NULL);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static int file_exists_w(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* 1 when the game is there (already, or now). 0: the player said no or the
 * copy failed, and was told why; the launcher ends. */
int launcher_iso_ensure_game(void)
{
    wchar_t exe[MAX_PATH], xbe[MAX_PATH], *slash;
    wchar_t xbe_name[64];
    OPENFILENAMEW ofn;
    HANDLE t;

    if (getenv("RECOMP_GAME_DIR"))
        return 1;                           /* somebody else's arrangement */
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return 1;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return 1;
    *slash = 0;
    MultiByteToWideChar(CP_ACP, 0, LAUNCHER_GAME_XBE, -1, xbe_name, 64);
    _snwprintf(g_game_dir, MAX_PATH - 1, L"%s\\game", exe);
    _snwprintf(xbe, MAX_PATH - 1, L"%s\\%s", g_game_dir, xbe_name);
    if (file_exists_w(xbe))
        return 1;
#ifdef LAUNCHER_GAME_DIR
    {
        /* A development tree: the game where the title's main.c says. */
        wchar_t dev[MAX_PATH], rel[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, LAUNCHER_GAME_DIR, -1, rel, MAX_PATH);
        _snwprintf(dev, MAX_PATH - 1, L"%s\\%s\\%s", exe, rel, xbe_name);
        if (file_exists_w(dev))
            return 1;
    }
#endif

    if (MessageBoxW(NULL,
            L"The game's files are not here yet. This port does not include the game:\n"
            L"it needs an image of your own original disc (.iso / .xiso).\n\n"
            L"Die Spieldateien fehlen noch. Dieser Port enthaelt das Spiel nicht:\n"
            L"er braucht ein Image deiner eigenen Original-Disc (.iso / .xiso).\n\n"
            L"Choose the disc image now? / Jetzt das Disc-Image auswaehlen?",
            L"Disc image needed / Disc-Image benoetigt",
            MB_YESNO | MB_ICONINFORMATION) != IDYES)
        return 0;

    memset(&ofn, 0, sizeof ofn);
    g_iso_path[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Xbox disc image (*.iso, *.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    ofn.lpstrFile = g_iso_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Original disc image / Original-Disc-Image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn))
        return 0;

    memset(&g_job, 0, sizeof g_job);
    g_job.iso = INVALID_HANDLE_VALUE;
    t = CreateThread(NULL, 0, extract_thread, NULL, 0, NULL);
    if (!t)
        return 0;
    run_progress_window(GetModuleHandleW(NULL));
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    if (g_job.failed || !file_exists_w(xbe)) {
        MessageBoxW(NULL, g_job.failed ? g_job.error :
                    L"The image has no game on it / Auf dem Image ist kein Spiel.",
                    L"Disc image / Disc-Image", MB_OK | MB_ICONERROR);
        return 0;
    }
#ifdef LAUNCHER_XBE_SHA1
    {
        char hex[41];
        if (sha1_file(xbe, hex) && _stricmp(hex, LAUNCHER_XBE_SHA1) != 0) {
            MessageBoxW(NULL,
                L"The files are copied, but this disc is a different version from the one\n"
                L"this port was made from. The game will probably not start.\n\n"
                L"Die Dateien sind kopiert, aber diese Disc ist eine andere Version als die,\n"
                L"aus der dieser Port gemacht wurde. Das Spiel startet wahrscheinlich nicht.",
                L"Disc image / Disc-Image", MB_OK | MB_ICONWARNING);
            return 1;
        }
    }
#endif
    {
        wchar_t done[200];
        _snwprintf(done, 199, L"%u files copied. / %u Dateien kopiert.", g_job.files, g_job.files);
        done[199] = 0;
        MessageBoxW(NULL, done, L"Disc image / Disc-Image", MB_OK | MB_ICONINFORMATION);
    }
    return 1;
}
