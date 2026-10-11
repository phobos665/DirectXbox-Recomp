/*
 * kernel_file_rename - NtSetInformationFile(FileRenameInformation) on the
 * host file backends (src/kernel/kernel_file.c, xbox_RenameFileByHandle).
 *
 * The class was unhandled: a title that writes a file under a temporary name
 * and renames it into place got STATUS_NOT_IMPLEMENTED, the final name never
 * appeared, and the title rebuilt the file on every visit (upstream #190).
 *
 * Checked, on whichever backend this host builds (Win32 or POSIX):
 *   - a full guest path resolves through the path table, as NtCreateFile's
 *     does (D:\ is the game folder, T:\ is <save>/TitleData);
 *   - ReplaceIfExists = FALSE on an existing target is
 *     STATUS_OBJECT_NAME_COLLISION and changes nothing;
 *   - ReplaceIfExists = TRUE replaces it;
 *   - a bare name with no root renames within the file's own directory;
 *   - a name relative to RootDirectory lands in that directory;
 *   - the handle follows the file: it still reads, and a delete-on-close set
 *     after the renames removes the final name (on POSIX that is by path, so
 *     the handle's recorded path has to have followed).
 * Not checked here: the bridge's unpacking of the guest FILE_RENAME_
 * INFORMATION (kernel_bridge.c), which needs a guest memory layout.
 *
 * No game files; a temporary game folder and a temporary save folder, both
 * removed afterwards. The save folder is passed to xbox_path_init, so the
 * per-user saves are never touched.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEP "/"
#endif

void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_ST(name, got, want) \
    do { NTSTATUS g_ = (got), w_ = (want); if (g_ != w_) { \
        printf("FAIL: %s (status 0x%08X, want 0x%08X)\n", name, \
               (unsigned)g_, (unsigned)w_); failures++; } } while (0)

static char s_game[512], s_save[512];

/* ---- host helpers --------------------------------------------------------- */

static void host_join(char *out, size_t n, const char *dir, const char *rel)
{
    snprintf(out, n, "%s" SEP "%s", dir, rel);
}

static int host_exists(const char *dir, const char *rel)
{
    char p[1024];
    host_join(p, sizeof p, dir, rel);
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

static void host_write(const char *dir, const char *rel, const char *text)
{
    char p[1024];
    FILE *f;
    host_join(p, sizeof p, dir, rel);
    f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* The file's contents, or "" when it cannot be read. */
static const char *host_read(const char *dir, const char *rel)
{
    static char buf[64];
    char p[1024];
    size_t n = 0;
    FILE *f;
    host_join(p, sizeof p, dir, rel);
    buf[0] = 0;
    f = fopen(p, "rb");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    return buf;
}

static void host_mkdir(const char *dir, const char *rel)
{
    char p[1024];
    host_join(p, sizeof p, dir, rel);
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static void host_rmtree(const char *path)
{
#if defined(_WIN32)
    char pat[1024], child[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", path);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                host_rmtree(child);
            else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    char child[1024];
    struct dirent *e;
    struct stat st;
    DIR *d = opendir(path);
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                host_rmtree(child);
            else
                unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

static int make_temp_dirs(void)
{
#if defined(_WIN32)
    char tmp[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmp))
        return 0;
    snprintf(s_game, sizeof s_game, "%sxboxrecomp-rename-%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    snprintf(s_save, sizeof s_save, "%s-save", s_game);
    return CreateDirectoryA(s_game, NULL) && CreateDirectoryA(s_save, NULL);
#else
    char tmpl[] = "/tmp/xboxrecomp-rename-XXXXXX";
    if (!mkdtemp(tmpl))
        return 0;
    snprintf(s_game, sizeof s_game, "%s", tmpl);
    snprintf(s_save, sizeof s_save, "%s-save", s_game);
    return mkdir(s_save, 0755) == 0;
#endif
}

/* ---- guest-side helpers --------------------------------------------------- */

static void make_oa(XBOX_OBJECT_ATTRIBUTES *oa, XBOX_ANSI_STRING *s,
                    const char *path, HANDLE root)
{
    s->Buffer = (PCHAR)path;
    s->Length = (USHORT)strlen(path);
    s->MaximumLength = (USHORT)(s->Length + 1);
    oa->RootDirectory = root;
    oa->ObjectName = s;
    oa->Attributes = 0;
}

/* The file: what XAPI's MoveFile asks for (DELETE), plus read so it can be
 * checked. The directory: what a title lists one with. A directory handle
 * that held DELETE without sharing it would make the rename into it a
 * sharing violation on Windows, which is NT's rule too. */
#define FILE_ACCESS (XBOX_GENERIC_READ | XBOX_DELETE | XBOX_SYNCHRONIZE)
#define DIR_ACCESS  (XBOX_GENERIC_READ | XBOX_SYNCHRONIZE)

static NTSTATUS guest_open(const char *path, ACCESS_MASK access, ULONG options,
                           HANDLE *out)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING s;
    XBOX_IO_STATUS_BLOCK ios;
    make_oa(&oa, &s, path, NULL);
    memset(&ios, 0, sizeof ios);
    *out = NULL;
    return xbox_NtCreateFile(out, access, &oa, &ios, NULL, 0,
                             0x3 /* share read|write */, XBOX_FILE_OPEN,
                             options | XBOX_FILE_SYNCHRONOUS_IO_NONALERT);
}

/* The open file's contents from the start, through its handle. The host
 * cannot open it by name meanwhile: the handle holds DELETE unshared. */
static const char *guest_read(HANDLE h)
{
    static char buf[16];
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    memset(buf, 0, sizeof buf);
    memset(&ios, 0, sizeof ios);
    if (xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, sizeof buf - 1, &zero)
            != STATUS_SUCCESS)
        buf[0] = 0;
    return buf;
}

static NTSTATUS guest_rename(HANDLE h, const char *name, HANDLE root, int replace)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING s;
    make_oa(&oa, &s, name, root);
    return xbox_RenameFileByHandle(h, &oa, (BOOLEAN)(replace != 0));
}

int main(void)
{
    HANDLE h = NULL, dir = NULL;
    char save_title[1024];

    printf("kernel_file_rename: running\n");
    if (!make_temp_dirs()) {
        printf("FAIL: could not make temporary folders\n");
        return 2;
    }
    xbox_path_init(s_game, s_save);
    host_join(save_title, sizeof save_title, s_save, "TitleData");
    host_mkdir(s_save, "TitleData");
    host_mkdir(s_game, "sub");
    host_write(s_game, "a.tmp", "payload");
    host_write(s_game, "c.dat", "old");

    CHECK_ST("open D:\\a.tmp", guest_open("D:\\a.tmp", FILE_ACCESS, 0, &h),
             STATUS_SUCCESS);
    if (!h || h == INVALID_HANDLE_VALUE) {
        printf("FAIL: no handle, nothing more to check\n");
        host_rmtree(s_game); host_rmtree(s_save);
        return 1;
    }

    /* A full guest path, through the path table. */
    CHECK_ST("rename to D:\\b.dat", guest_rename(h, "D:\\b.dat", NULL, 0), STATUS_SUCCESS);
    CHECK("b.dat exists", host_exists(s_game, "b.dat"));
    CHECK("a.tmp gone", !host_exists(s_game, "a.tmp"));

    /* The handle still reads the file it had open. */
    CHECK("read after rename returns the data", strcmp(guest_read(h), "payload") == 0);

    /* An existing target without ReplaceIfExists: a collision, nothing moves. */
    CHECK_ST("rename onto c.dat, no replace",
             guest_rename(h, "D:\\c.dat", NULL, 0), STATUS_OBJECT_NAME_COLLISION);
    CHECK("collision keeps b.dat", host_exists(s_game, "b.dat"));
    CHECK("collision keeps c.dat's data", strcmp(host_read(s_game, "c.dat"), "old") == 0);

    /* With it: replaced. */
    CHECK_ST("rename onto c.dat, replace",
             guest_rename(h, "D:\\c.dat", NULL, 1), STATUS_SUCCESS);
    CHECK("replace leaves one c.dat", host_exists(s_game, "c.dat"));
    CHECK("replace removes b.dat", !host_exists(s_game, "b.dat"));
    CHECK("the handle reads its own data at c.dat", strcmp(guest_read(h), "payload") == 0);

    /* A bare name, no root: within the file's own directory. */
    CHECK_ST("rename to bare d.dat", guest_rename(h, "d.dat", NULL, 0), STATUS_SUCCESS);
    CHECK("d.dat beside it", host_exists(s_game, "d.dat"));
    CHECK("c.dat gone", !host_exists(s_game, "c.dat"));

    /* Relative to RootDirectory. */
    CHECK_ST("open D:\\sub", guest_open("D:\\sub", DIR_ACCESS, XBOX_FILE_DIRECTORY_FILE, &dir),
             STATUS_SUCCESS);
    if (dir && dir != INVALID_HANDLE_VALUE) {
        CHECK_ST("rename relative to D:\\sub", guest_rename(h, "e.dat", dir, 0),
                 STATUS_SUCCESS);
        CHECK("sub/e.dat exists", host_exists(s_game, "sub" SEP "e.dat"));
        CHECK("d.dat gone", !host_exists(s_game, "d.dat"));
        xbox_NtClose(dir);
    }

    /* A different rule of the path table: T:\ is <save>/TitleData. */
    CHECK_ST("rename to T:\\moved.dat", guest_rename(h, "T:\\moved.dat", NULL, 0),
             STATUS_SUCCESS);
    CHECK("moved.dat under the save folder's TitleData",
          host_exists(save_title, "moved.dat"));

    /* Delete-on-close after the renames removes the final name. */
    {
        XBOX_FILE_DISPOSITION_INFORMATION di;
        XBOX_IO_STATUS_BLOCK ios;
        memset(&ios, 0, sizeof ios);
        di.DeleteFile = TRUE;
        CHECK_ST("delete-on-close",
                 xbox_NtSetInformationFile(h, &ios, &di, sizeof di,
                                           XboxFileDispositionInformation),
                 STATUS_SUCCESS);
    }
    xbox_NtClose(h);
    CHECK("delete-on-close removed the renamed file", !host_exists(save_title, "moved.dat"));

    /* No file: the handle a title passes on after an open it never checked. */
    CHECK_ST("invalid handle refused", guest_rename(INVALID_HANDLE_VALUE, "x.dat", NULL, 0),
             STATUS_INVALID_HANDLE);

    host_rmtree(s_game);
    host_rmtree(s_save);

    if (failures == 0) {
        printf("kernel_file_rename: ALL PASS\n");
        return 0;
    }
    printf("kernel_file_rename: %d FAILURE(S)\n", failures);
    return 1;
}
