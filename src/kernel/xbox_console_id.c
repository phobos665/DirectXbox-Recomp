/**
 * The console's own identity, from the player's backup of a real console, or
 * generated once for this install.
 *
 * Without either, every install is the same made-up console: serial
 * 100000000000, a random Ethernet address per save folder, and zeroed online
 * and hard disk keys. That is enough for System Link between recompiled
 * builds. Xbox Live is not: the title's own XOnline code creates the
 * console's machine account from the serial and the online key, and
 * Insignia registers a console by its keys and refuses one whose hard disk
 * key is zero. The player's own console's identity works (they back it up
 * with EvoX, UnleashX or XBMC: an eeprom.bin, and the HDD key the same tools
 * show), and so does one made up properly, as hakuX makes its consoles:
 *
 *     console_identity = generate | backup      [RECOMP_CONSOLE_IDENTITY]
 *
 * generate (the default when syslink = nat, Xbox Live) makes a console in the
 * settings folder's console/ when no backup is found anywhere below: twelve
 * random digits for the serial, 00:50:F2 and three random bytes for the
 * Ethernet address, sixteen random bytes for the online key, and hakuX's HDD
 * key, which every generated console shares (see xbox_console_generate).
 * Made once and kept: to Insignia a new identity is a new console, and a
 * stream of them looks like ban evasion, so nothing here ever replaces one.
 * backup (the default otherwise) only ever uses a backup.
 *
 * Where it is looked for, first found wins:
 *
 *     console_dir = <folder>        [RECOMP_CONSOLE_DIR]   (off: none at all)
 *     <the game folder>/../xboxbackup/
 *     <the per-user settings folder>/console/
 *     ... and, with console_identity = generate, a new console made there
 *
 * In that folder:
 *
 *     eeprom.bin    256 bytes, required. Its factory and user sections must
 *                   carry good checksums, or it is not used.
 *     hddinfo.txt   EvoX's, read for its "XboxHDKey" line and the drive's
 *                   serial and model.
 *     hd_key.txt    or the HDD key alone, 32 hex digits.
 *     generated     only in a generated console's folder, saying it is one.
 *
 * and for a backup the HDD key from keys.ini (hd_key = ...) or
 * RECOMP_XBOX_HD_KEY wins over both. The key in eeprom.bin itself is
 * encrypted with the BIOS's EEPROM key, which is not needed when the
 * decrypted key is given.
 *
 * And disk.bin, the first 512 KB of the console's drive (EvoX), whose
 * Xbox Live accounts are copied onto the runtime's disk image once
 * (xbox_console_import_accounts).
 *
 * What it changes, when found: the serial, Ethernet address and online key
 * ExQueryNonVolatileSetting answers (kernel_xbox.c), XboxHDKey and the HAL's
 * disk serial and model (kernel_bridge.c), and the card's Ethernet address
 * (xbox_nic.c; RECOMP_SYSLINK_MAC still wins). A save the title signs with a
 * console-bound key is then signed as that console signs it, so saves copied
 * from it verify, and saves made earlier under the zero key may not.
 *
 * All of it is secret to one console. Nothing here prints a key, the serial
 * or the full Ethernet address, only where they came from.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_console_id.h"
#include "kernel.h"          /* xbox_path_game_dir_utf8 */
#include "recomp_config.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#define SEP "\\"
#define strtok_r strtok_s
#else
#include <strings.h>
#define SEP "/"
#define _stricmp strcasecmp
#endif

/* EEPROM layout (xboxdevwiki "EEPROM"): an encrypted security section, then
 * the factory section and the user section, each led by its checksum. */
#define EE_FACTORY_SUM   0x30
#define EE_FACTORY       0x34
#define EE_FACTORY_END   0x60
#define EE_SERIAL        0x34
#define EE_MAC           0x40
#define EE_ONLINE_KEY    0x48
#define EE_VIDEO_STD     0x58
#define EE_USER_SUM      0x60
#define EE_USER          0x64
#define EE_USER_END      0xC0

uint32_t xbox_eeprom_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t hi = 0, lo = 0, i;
    uint64_t s;

    for (i = 0; i + 4 <= len; i += 4) {
        uint32_t w = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                     (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
        s = (uint64_t)lo + w;
        lo = (uint32_t)s;
        hi += (uint32_t)(s >> 32);
    }
    s = (uint64_t)hi + lo;
    return ~(uint32_t)(s + (s >> 32));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int file_in_dir(const char *dir, const char *name)
{
    char path[1100];
    FILE *f;

    snprintf(path, sizeof path, "%s" SEP "%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int is_dir_with_eeprom(const char *dir)
{
    return file_in_dir(dir, "eeprom.bin");
}

/* ---- a console of this install's own ---------------------------------- */

/* hakuX's (native/kernel/ex.c, k_legacy_hd_key; GPL-3.0-or-later): the ASCII
 * digits 0000000001, then 00 50 F2 00 00 01. Shared by every generated
 * console, on purpose. The machine key is the online key under a keystream
 * from the HDD key, so a unique online key still makes a unique console to
 * Insignia, which finds a console by its serial; hakuX players sign in with
 * this key; and one key everywhere lets console-bound saves move between
 * hakuX and these builds. What it costs: every install is one console to
 * content locked to a console, and an account key on the disk is protected
 * only by a key anyone can read here. Non-zero, which Insignia requires. */
const uint8_t xbox_console_generated_hd_key[16] = {
    '0', '0', '0', '0', '0', '0', '0', '0', '0', '1',
    0x00, 0x50, 0xF2, 0x00, 0x00, 0x01,
};

static int random_bytes(uint8_t *p, size_t n)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f) {
        got = fread(p, 1, n, f);
        fclose(f);
    }
    return got == n;
#endif
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Write a file whole or not at all: into name.new, then renamed over. */
static int write_whole(const char *dir, const char *name, const void *data, size_t len)
{
    char path[1100], tmp[1110];
    FILE *f;
    int ok;

    snprintf(path, sizeof path, "%s" SEP "%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        remove(tmp);
    return ok;
}

int xbox_console_generate(const char *dir)
{
    uint8_t ee[256], r[64];
    char hex[40];
    size_t used = 0;
    int i, n;

    /* Anything of a console's already here is the player's, or an identity
     * Insignia may know: never written over. */
    if (file_in_dir(dir, "eeprom.bin") || file_in_dir(dir, "hd_key.txt") ||
        file_in_dir(dir, "hddinfo.txt"))
        return 0;
    if (!recomp_make_dirs(dir))
        return 0;
    memset(ee, 0, sizeof ee);
    if (!random_bytes(r, sizeof r))
        return 0;

    /* Twelve digits, each from a byte below 250 so none is likelier. */
    for (n = 0; n < 12; ) {
        if (used == sizeof r) {
            if (!random_bytes(r, sizeof r))
                return 0;
            used = 0;
        }
        if (r[used] < 250)
            ee[EE_SERIAL + n++] = (uint8_t)('0' + r[used] % 10);
        used++;
    }
    /* Microsoft's prefix, as a real console's, then 24 random bits, unicast. */
    ee[EE_MAC + 0] = 0x00; ee[EE_MAC + 1] = 0x50; ee[EE_MAC + 2] = 0xF2;
    if (!random_bytes(ee + EE_MAC + 3, 3) || !random_bytes(ee + EE_ONLINE_KEY, 16))
        return 0;
    put_le32(ee + EE_VIDEO_STD, 0x00400100u);         /* NTSC-M; the kernel
                                                       * answers the title's own */
    put_le32(ee + EE_FACTORY_SUM, xbox_eeprom_checksum(ee + EE_FACTORY,
                                                       EE_FACTORY_END - EE_FACTORY));
    /* The user section is the runtime's to fill (kernel_xbox.c lays its
     * settings over it on every read); only its checksum is needed here. */
    put_le32(ee + EE_USER_SUM, xbox_eeprom_checksum(ee + EE_USER, EE_USER_END - EE_USER));
    /* The security section (0x00-0x2F) stays zero: a real one is encrypted
     * with the BIOS's EEPROM key, which this runtime does not have, and the
     * HDD key it would hold is read from hd_key.txt instead. */

    for (i = 0; i < 16; i++)
        snprintf(hex + 2 * i, 3, "%02X", xbox_console_generated_hd_key[i]);
    hex[32] = '\n';
    /* eeprom.bin last: it is what makes the folder a console, so a failure
     * before it leaves nothing that is used. `generated` marks the console as
     * one of these rather than a backup (XboxConsoleId.generated). */
    if (write_whole(dir, "hd_key.txt", hex, 33) &&
        write_whole(dir, "generated", "console_identity = generate\n", 28) &&
        write_whole(dir, "eeprom.bin", ee, sizeof ee))
        return 1;
    {
        static const char *const made[] = { "hd_key.txt", "generated" };
        char path[1100];
        for (i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s" SEP "%s", dir, made[i]);
            remove(path);
        }
    }
    return 0;
}

static int want_generated(void)
{
    const char *v = recomp_config_lookup("RECOMP_CONSOLE_IDENTITY", "console_identity");

    if (v && *v)
        return _stricmp(v, "generate") == 0;
    v = recomp_config_lookup("RECOMP_SYSLINK", "syslink");
    return v && _stricmp(v, "nat") == 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 16 bytes of hex, with or without spaces between the pairs. */
static int parse_key16(const char *s, uint8_t out[16])
{
    int n = 0;

    while (*s && n < 16) {
        int hi, lo;
        while (*s == ' ' || *s == '\t' || *s == ':' || *s == '-')
            s++;
        hi = hexval((unsigned char)s[0]);
        lo = hi < 0 ? -1 : hexval((unsigned char)s[1]);
        if (lo < 0)
            break;
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return n == 16;
}

/* The value after `name` on a line of a key = value or "Name : value" file. */
static int line_value(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);

    while (*line == ' ' || *line == '\t')
        line++;
    if (strncmp(line, name, n) != 0)
        return 0;
    line += n;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '=' || *line == ':')
        line++;
    while (*line == ' ' || *line == '\t')
        line++;
    *value = line;
    return 1;
}

static void trim_copy(char *out, size_t n, const char *v)
{
    size_t len;

    snprintf(out, n, "%s", v);
    len = strlen(out);
    while (len && isspace((unsigned char)out[len - 1]))
        out[--len] = 0;
}

static int key_from_file(const char *path, const char *name, uint8_t out[16])
{
    char line[512];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        const char *v;
        if (name ? line_value(line, name, &v) : (v = line, 1))
            found = parse_key16(v, out);
    }
    fclose(f);
    return found;
}

static void read_hddinfo(XboxConsoleId *id, const char *path, const char **key_source)
{
    char line[512];
    FILE *f = fopen(path, "r");

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        const char *v;
        if (line_value(line, "XboxHDKey", &v) && !id->have_hd_key &&
            parse_key16(v, id->hd_key)) {
            id->have_hd_key = 1;
            *key_source = "hddinfo.txt";
        } else if (line_value(line, "Disk Serial Number", &v)) {
            trim_copy(id->disk_serial, sizeof id->disk_serial, v);
        } else if (line_value(line, "Disk Model Number", &v)) {
            trim_copy(id->disk_model, sizeof id->disk_model, v);
        }
    }
    fclose(f);
}

/* The folder holding `path`, worked out from the text: "." and ".." are
 * collapsed as written, so a game folder that is a symlink (game -> the
 * toolkit's games/<title>) still has the project as its parent, where the
 * OS, following the link, would say the toolkit's games/. */
static void lexical_parent(const char *path, char *out, size_t n)
{
    char buf[1024], *parts[128], *p, *save = NULL;
    int count = 0, i, absolute;
    size_t len;

    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf; *p; p++)
        if (*p == '\\')
            *p = '/';
    absolute = buf[0] == '/';
    for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (strcmp(p, ".") == 0)
            continue;
        if (strcmp(p, "..") == 0 && count > 0 && strcmp(parts[count - 1], "..") != 0) {
            count--;
            continue;
        }
        if (count < (int)(sizeof parts / sizeof parts[0]))
            parts[count++] = p;
    }
    if (count > 0)
        count--;                          /* the parent */
    out[0] = 0;
    len = 0;
    if (absolute)
        len += (size_t)snprintf(out, n, "/");
    for (i = 0; i < count && len < n; i++)
        len += (size_t)snprintf(out + len, n - len, "%s%s", i ? SEP : "", parts[i]);
    if (!out[0])
        snprintf(out, n, ".");
}

static int find_dir(char *out, size_t n, const char **how)
{
    const char *v = recomp_config_lookup("RECOMP_CONSOLE_DIR", "console_dir");
    char base[1024];

    if (v && *v) {
        if (_stricmp(v, "off") == 0 || strcmp(v, "0") == 0) {
            *how = "off";
            return 0;
        }
        snprintf(out, n, "%s", v);
        *how = "console_dir";
        return is_dir_with_eeprom(out);
    }
    if (xbox_path_game_dir_utf8(base, sizeof base)) {
        char parent[1024];
        lexical_parent(base, parent, sizeof parent);
        snprintf(out, n, "%s" SEP "xboxbackup", parent);
        if (is_dir_with_eeprom(out)) {
            *how = "beside the game folder";
            return 1;
        }
    }
    if (recomp_config_user_dir(base, sizeof base)) {
        snprintf(out, n, "%s" SEP "console", base);
        if (is_dir_with_eeprom(out)) {
            *how = file_in_dir(out, "generated") ? "generated earlier" : "the settings folder";
            return 1;
        }
        if (want_generated()) {
            if (xbox_console_generate(out)) {
                *how = "generated now";
                return 1;
            }
            fprintf(stderr, "[XBOX] console identity: could not generate one in %s "
                            "(%s); the made-up console instead\n", out,
                    file_in_dir(out, "hd_key.txt") || file_in_dir(out, "hddinfo.txt")
                        ? "it holds part of a backup with no eeprom.bin"
                        : "cannot write there");
        }
    }
    *how = NULL;
    return 0;
}

static int load(XboxConsoleId *id)
{
    char path[1100], dir[1024];
    const char *how = NULL, *key_source = NULL, *env;
    FILE *f;
    size_t got;
    int i;

    memset(id, 0, sizeof *id);
    if (!find_dir(dir, sizeof dir, &how)) {
        if (how && strcmp(how, "off") == 0)
            fprintf(stderr, "[XBOX] console identity: off (console_dir = off)\n");
        else if (how)
            fprintf(stderr, "[XBOX] console identity: no eeprom.bin in console_dir "
                            "%s; the made-up console instead\n", dir);
        return 0;
    }
    snprintf(id->dir, sizeof id->dir, "%s", dir);
    id->generated = file_in_dir(dir, "generated");
    if (strcmp(how, "generated now") == 0)
        fprintf(stderr, "[XBOX] console identity: generated a console for this "
                        "install in %s (console_identity = generate, the default "
                        "with syslink = nat). It is kept for good: Insignia knows "
                        "a console by it, so keep that folder. Saves a title tied "
                        "to the made-up console before it (zero HDD key) will "
                        "not verify under it.\n", dir);

    snprintf(path, sizeof path, "%s" SEP "eeprom.bin", dir);
    f = fopen(path, "rb");
    got = f ? fread(id->eeprom, 1, sizeof id->eeprom, f) : 0;
    if (f && fgetc(f) != EOF)
        got = 0;                          /* longer than an EEPROM */
    if (f)
        fclose(f);
    if (got != sizeof id->eeprom) {
        fprintf(stderr, "[XBOX] console identity: %s is not a 256-byte EEPROM; "
                        "not used\n", path);
        return 0;
    }
    if (xbox_eeprom_checksum(id->eeprom + EE_FACTORY, EE_FACTORY_END - EE_FACTORY) !=
            le32(id->eeprom + EE_FACTORY_SUM) ||
        xbox_eeprom_checksum(id->eeprom + EE_USER, EE_USER_END - EE_USER) !=
            le32(id->eeprom + EE_USER_SUM)) {
        fprintf(stderr, "[XBOX] console identity: %s fails its checksums (not an "
                        "EEPROM backup, or damaged); not used\n", path);
        return 0;
    }
    memcpy(id->serial, id->eeprom + EE_SERIAL, 12);
    for (i = 0; i < 12; i++)
        if (!isdigit((unsigned char)id->serial[i])) {
            fprintf(stderr, "[XBOX] console identity: %s has no serial number; "
                            "not used\n", path);
            return 0;
        }
    memcpy(id->mac, id->eeprom + EE_MAC, 6);
    memcpy(id->online_key, id->eeprom + EE_ONLINE_KEY, 16);

    /* The HDD key: the environment, keys.ini, then the backup's own files.
     * A generated console is its files alone: a key meant for a real console
     * would pair that console's key with a made-up serial. */
    env = id->generated ? NULL : getenv("RECOMP_XBOX_HD_KEY");
    if (env && *env) {
        if (parse_key16(env, id->hd_key)) {
            id->have_hd_key = 1;
            key_source = "RECOMP_XBOX_HD_KEY";
        } else {
            fprintf(stderr, "[XBOX] RECOMP_XBOX_HD_KEY is not 32 hex digits; ignored\n");
        }
    }
    if (!id->have_hd_key && !id->generated) {
        char udir[1024];
        if (recomp_config_user_dir(udir, sizeof udir)) {
            snprintf(path, sizeof path, "%s" SEP "keys.ini", udir);
            if (key_from_file(path, "hd_key", id->hd_key)) {
                id->have_hd_key = 1;
                key_source = "keys.ini";
            }
        }
    }
    snprintf(path, sizeof path, "%s" SEP "hddinfo.txt", dir);
    read_hddinfo(id, path, &key_source);
    if (!id->have_hd_key) {
        snprintf(path, sizeof path, "%s" SEP "hd_key.txt", dir);
        if (key_from_file(path, NULL, id->hd_key)) {
            id->have_hd_key = 1;
            key_source = "hd_key.txt";
        }
    }
    if (id->have_hd_key) {
        static const uint8_t zero[16];
        if (memcmp(id->hd_key, zero, 16) == 0)
            fprintf(stderr, "[XBOX] console identity: the HDD key is all zeros; "
                            "Insignia refuses such a console\n");
    }

    fprintf(stderr, "[XBOX] console identity from %s (%s): eeprom.bin, Ethernet "
                    "address %02X:%02X:%02X:..., %s%s%s\n",
            dir, how, id->mac[0], id->mac[1], id->mac[2],
            id->have_hd_key ? "HDD key from " : "no HDD key (keys.ini hd_key, or "
                                                "hddinfo.txt beside eeprom.bin)",
            id->have_hd_key ? key_source : "",
            id->disk_serial[0] ? ", and the drive's serial and model" : "");
    fflush(stderr);
    return 1;
}

static XboxConsoleId s_id;
static int s_state;          /* 0 not looked, 1 found, -1 none */

const XboxConsoleId *xbox_ConsoleId(void)
{
    if (!s_state)
        s_state = load(&s_id) ? 1 : -1;
    return s_state > 0 ? &s_id : NULL;
}

void xbox_console_id_reset(void)
{
    s_state = 0;
    memset(&s_id, 0, sizeof s_id);
}

/* ---- the Xbox Live accounts ------------------------------------------- */

#define ACCT_FIRST  8           /* the index */
#define ACCT_LAST   15
#define SECTOR      512u

/* What an account sector holds: 0 nothing, 1 the machine account (named
 * SN.<serial>), 2 a user account. The record starts 0x0C into its sector,
 * its name at +0x0C, its Kerberos domain at +0x24. */
static int account_kind(const uint8_t *sec)
{
    const uint8_t *rec = sec + 0x0C;
    uint32_t i;

    for (i = 0; i < SECTOR && !sec[i]; i++)
        ;
    if (i == SECTOR)
        return 0;
    if (!rec[0x24])
        return 0;               /* no domain: not an account record */
    return memcmp(rec + 0x0C, "SN.", 3) == 0 ? 1 : 2;
}

static int any_account(const uint8_t *area)
{
    uint32_t s;

    for (s = ACCT_FIRST + 1; s <= ACCT_LAST; s++)
        if (account_kind(area + (s - ACCT_FIRST) * SECTOR))
            return 1;
    return 0;
}

void xbox_console_import_accounts(const char *image_path)
{
    enum { N = (ACCT_LAST - ACCT_FIRST + 1) * SECTOR };
    static uint8_t from[N], onto[N];
    const XboxConsoleId *id = xbox_ConsoleId();
    const char *mode = recomp_config_lookup("RECOMP_CONSOLE_ACCOUNTS", "console_accounts");
    int replace = mode && _stricmp(mode, "replace") == 0;
    int machine = 0, users = 0;
    char path[1100];
    uint32_t s;
    FILE *f;
    size_t got;

    if (!id || !image_path || (mode && _stricmp(mode, "off") == 0))
        return;
    snprintf(path, sizeof path, "%s" SEP "disk.bin", id->dir);
    f = fopen(path, "rb");
    if (!f)
        return;                 /* no drive backup: nothing to bring */
    got = fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) == 0 ? fread(from, 1, N, f) : 0;
    fclose(f);
    if (got != N || !any_account(from)) {
        fprintf(stderr, "[XBOX] Xbox Live accounts: none in %s (a backup from "
                        "before signing up?)\n", path);
        return;
    }

    f = fopen(image_path, "r+b");
    if (!f) {
        fprintf(stderr, "[XBOX] Xbox Live accounts: cannot open %s\n", image_path);
        return;
    }
    got = fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) == 0 ? fread(onto, 1, N, f) : 0;
    if (got == N && any_account(onto) && !replace) {
        fclose(f);
        /* The index (sector 8) is the title's to update every run; only
         * different accounts are worth a word. */
        if (memcmp(onto + SECTOR, from + SECTOR, N - SECTOR) != 0)
            fprintf(stderr, "[XBOX] Xbox Live accounts: the disk already has its "
                            "own, kept (console_accounts = replace takes "
                            "disk.bin's)\n");
        return;
    }
    if (fseek(f, (long)(ACCT_FIRST * SECTOR), SEEK_SET) != 0 ||
        fwrite(from, 1, N, f) != N) {
        fclose(f);
        fprintf(stderr, "[XBOX] Xbox Live accounts: cannot write %s\n", image_path);
        return;
    }
    fclose(f);
    for (s = ACCT_FIRST + 1; s <= ACCT_LAST; s++) {
        int k = account_kind(from + (s - ACCT_FIRST) * SECTOR);
        machine += k == 1;
        users += k == 2;
    }
    fprintf(stderr, "[XBOX] Xbox Live accounts copied from disk.bin onto the "
                    "disk (sectors %d-%d): %d machine account, %d user "
                    "account%s\n", ACCT_FIRST, ACCT_LAST, machine, users,
            users == 1 ? "" : "s");
    fflush(stderr);
}
