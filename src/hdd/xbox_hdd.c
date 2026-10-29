/**
 * An Xbox hard disk image and its FATX volumes, read. See xbox_hdd.h.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_hdd.h"

#ifdef _WIN32
#include <direct.h>
#define SEP '\\'
#define fseeko _fseeki64
#define ftello _ftelli64
static int make_dir(const char *p) { return _mkdir(p); }
#else
#include <sys/stat.h>
#define SEP '/'
static int make_dir(const char *p) { return mkdir(p, 0755); }
#endif

static void say(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    if (!err || !n)
        return;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- the disk: raw or qcow2 ------------------------------------------- */

struct XboxHdd {
    FILE     *f;
    uint64_t  size;            /* the guest disk's */
    int       qcow2;
    uint32_t  cluster_bits;
    uint32_t  l1_size;
    uint64_t *l1;              /* host-endian */
    uint64_t  l2_at;           /* the cached table's file offset, 0 none */
    uint64_t *l2;
    char      error[160];
};

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int file_read(FILE *f, uint64_t off, void *buf, size_t n)
{
    return fseeko(f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

#define QCOW_OFFSET_MASK 0x00FFFFFFFFFFFE00ull
#define QCOW_COMPRESSED  (1ull << 62)
#define QCOW_ZERO        1ull

static int qcow2_open(XboxHdd *h, const uint8_t *hd, char *err, size_t err_n)
{
    uint32_t version = be32(hd + 4), i;
    uint64_t l1_off;
    uint8_t *raw;

    if (version != 2 && version != 3) {
        say(err, err_n, "qcow2 version %u is not supported", version);
        return 0;
    }
    if (be64(hd + 8) != 0) {
        say(err, err_n, "the image has a backing file; give the whole image (qemu-img convert)");
        return 0;
    }
    if (be32(hd + 32) != 0) {
        say(err, err_n, "the image is encrypted");
        return 0;
    }
    if (version == 3) {
        uint64_t incompatible = be64(hd + 72);
        /* bit 0 dirty (readable), 3 compression type (only for compressed
         * clusters, refused below); anything else is a layout this does not read. */
        if (incompatible & ~((1ull << 0) | (1ull << 3))) {
            say(err, err_n, "the image uses qcow2 features this does not read (0x%llx)",
                (unsigned long long)incompatible);
            return 0;
        }
    }
    h->cluster_bits = be32(hd + 20);
    if (h->cluster_bits < 9 || h->cluster_bits > 21) {
        say(err, err_n, "qcow2 cluster size 2^%u is not valid", h->cluster_bits);
        return 0;
    }
    h->size = be64(hd + 24);
    h->l1_size = be32(hd + 36);
    l1_off = be64(hd + 40);
    if (!h->l1_size || h->l1_size > (1u << 24)) {
        say(err, err_n, "qcow2 L1 table size %u is not valid", h->l1_size);
        return 0;
    }
    raw = malloc((size_t)h->l1_size * 8);
    h->l1 = malloc((size_t)h->l1_size * 8);
    h->l2 = malloc((size_t)1 << h->cluster_bits);
    if (!raw || !h->l1 || !h->l2 || !file_read(h->f, l1_off, raw, (size_t)h->l1_size * 8)) {
        free(raw);
        say(err, err_n, "cannot read the qcow2 L1 table");
        return 0;
    }
    for (i = 0; i < h->l1_size; i++)
        h->l1[i] = be64(raw + 8 * i);
    free(raw);
    h->qcow2 = 1;
    return 1;
}

XboxHdd *xbox_hdd_open(const char *path, char *err, size_t err_n)
{
    uint8_t hd[112];
    XboxHdd *h = calloc(1, sizeof *h);

    if (!h)
        return NULL;
    h->f = fopen(path, "rb");
    if (!h->f) {
        say(err, err_n, "cannot open %s: %s", path, strerror(errno));
        free(h);
        return NULL;
    }
    memset(hd, 0, sizeof hd);
    if (fread(hd, 1, sizeof hd, h->f) >= 72 && be32(hd) == 0x514649FBu) {   /* "QFI\xfb" */
        if (!qcow2_open(h, hd, err, err_n)) {
            xbox_hdd_close(h);
            return NULL;
        }
    } else {
        fseeko(h->f, 0, SEEK_END);
        h->size = (uint64_t)ftello(h->f);
    }
    return h;
}

uint64_t xbox_hdd_size(const XboxHdd *h) { return h->size; }
int xbox_hdd_is_qcow2(const XboxHdd *h) { return h->qcow2; }
const char *xbox_hdd_error(const XboxHdd *h) { return h->error; }

void xbox_hdd_close(XboxHdd *h)
{
    if (!h)
        return;
    if (h->f)
        fclose(h->f);
    free(h->l1);
    free(h->l2);
    free(h);
}

/* Within one qcow2 cluster. */
static int qcow2_read(XboxHdd *h, uint64_t off, uint8_t *buf, size_t n)
{
    uint64_t cs = 1ull << h->cluster_bits, l2_entries = cs / 8;
    uint64_t cluster = off >> h->cluster_bits, l1i = cluster / l2_entries;
    uint64_t l2i = cluster % l2_entries, l2_off, e;

    if (l1i >= h->l1_size || !(l2_off = h->l1[l1i] & QCOW_OFFSET_MASK)) {
        memset(buf, 0, n);
        return 1;
    }
    if (h->l2_at != l2_off) {
        uint8_t *raw = (uint8_t *)h->l2;
        uint64_t i;
        if (!file_read(h->f, l2_off, raw, (size_t)cs)) {
            snprintf(h->error, sizeof h->error, "cannot read a qcow2 L2 table");
            h->l2_at = 0;
            return 0;
        }
        for (i = 0; i < l2_entries; i++)
            h->l2[i] = be64(raw + 8 * i);
        h->l2_at = l2_off;
    }
    e = h->l2[l2i];
    if (e & QCOW_COMPRESSED) {
        snprintf(h->error, sizeof h->error, "the image has compressed clusters; "
                 "make an uncompressed copy (qemu-img convert -O qcow2)");
        return 0;
    }
    if ((e & QCOW_ZERO) || !(e & QCOW_OFFSET_MASK)) {
        memset(buf, 0, n);
        return 1;
    }
    if (!file_read(h->f, (e & QCOW_OFFSET_MASK) + (off & (cs - 1)), buf, n)) {
        snprintf(h->error, sizeof h->error, "cannot read the image at a cluster");
        return 0;
    }
    return 1;
}

int xbox_hdd_read(XboxHdd *h, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = buf;

    if (off >= h->size) {
        memset(buf, 0, n);
        return 1;
    }
    if (!h->qcow2) {
        size_t have = off + n > h->size ? (size_t)(h->size - off) : n;
        if (!file_read(h->f, off, p, have)) {
            snprintf(h->error, sizeof h->error, "cannot read the image");
            return 0;
        }
        memset(p + have, 0, n - have);
        return 1;
    }
    while (n) {
        uint64_t cs = 1ull << h->cluster_bits;
        size_t chunk = (size_t)(cs - (off & (cs - 1)));
        if (chunk > n)
            chunk = n;
        if (!qcow2_read(h, off, p, chunk))
            return 0;
        off += chunk;
        p += chunk;
        n -= chunk;
    }
    return 1;
}

/* ---- FATX --------------------------------------------------------------- */

#define FATX_SUPERBLOCK 0x1000u
#define FATX_ENTRY      64u
#define FATX_NAME_MAX   42u
#define FATX_ATTR_DIR   0x10u

struct XboxFatx {
    XboxHdd  *h;
    uint64_t  offset, size;
    uint32_t  cluster_size, root, entries;   /* entries: in the table */
    int       fat16;
    uint64_t  fat_at, data_at;
    uint8_t  *fat;                           /* the whole table */
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

XboxFatx *xbox_fatx_open(XboxHdd *h, uint64_t offset, uint64_t size, char *err, size_t err_n)
{
    uint8_t sb[16];
    uint64_t fat_bytes;
    XboxFatx *v;

    if (!xbox_hdd_read(h, offset, sb, sizeof sb)) {
        say(err, err_n, "%s", xbox_hdd_error(h));
        return NULL;
    }
    if (memcmp(sb, "FATX", 4) != 0) {
        say(err, err_n, "no FATX volume at 0x%llx (an Xbox disk image?)",
            (unsigned long long)offset);
        return NULL;
    }
    v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->h = h;
    v->offset = offset;
    v->size = size;
    v->cluster_size = le32(sb + 8) * 512u;
    v->root = le32(sb + 12);
    if (!v->cluster_size || v->cluster_size > (1u << 20) || (v->cluster_size & 511)) {
        say(err, err_n, "the FATX volume's cluster size is not valid");
        free(v);
        return NULL;
    }
    /* One entry a cluster and one reserved; 16-bit entries below 0xFFF0 of
     * them; the table rounded up to 4 KB; the clusters, from 1, after it. */
    v->entries = (uint32_t)(size / v->cluster_size) + 1u;
    v->fat16 = v->entries < 0xFFF0u;
    fat_bytes = (uint64_t)v->entries * (v->fat16 ? 2u : 4u);
    fat_bytes = (fat_bytes + 4095u) & ~(uint64_t)4095u;
    v->fat_at = offset + FATX_SUPERBLOCK;
    v->data_at = v->fat_at + fat_bytes;
    if (v->root < 1 || v->root >= v->entries) {
        say(err, err_n, "the FATX volume's root cluster is not valid");
        free(v);
        return NULL;
    }
    v->fat = malloc((size_t)fat_bytes);
    if (!v->fat || !xbox_hdd_read(h, v->fat_at, v->fat, (size_t)fat_bytes)) {
        say(err, err_n, "cannot read the FATX volume's table");
        xbox_fatx_close(v);
        return NULL;
    }
    return v;
}

void xbox_fatx_close(XboxFatx *v)
{
    if (!v)
        return;
    free(v->fat);
    free(v);
}

/* The cluster after c, or 0 at the end of a chain (or a broken one). */
static uint32_t next_cluster(const XboxFatx *v, uint32_t c)
{
    uint32_t e;

    if (c >= v->entries)
        return 0;
    if (v->fat16) {
        e = (uint32_t)v->fat[2 * c] | (uint32_t)v->fat[2 * c + 1] << 8;
        if (e >= 0xFFF0u)
            return 0;
    } else {
        e = le32(v->fat + 4 * (size_t)c);
        if (e >= 0xFFFFFFF0u)
            return 0;
    }
    return e >= 1 && e < v->entries ? e : 0;
}

static uint64_t cluster_at(const XboxFatx *v, uint32_t c)
{
    return v->data_at + (uint64_t)(c - 1u) * v->cluster_size;
}

/* A directory's whole contents, cluster by cluster, as one buffer. */
static uint8_t *read_dir(XboxFatx *v, uint32_t first, size_t *len)
{
    uint8_t *buf = NULL;
    size_t have = 0;
    uint32_t c = first, steps = 0;

    while (c && steps++ < v->entries) {
        uint8_t *g = realloc(buf, have + v->cluster_size);
        if (!g) {
            free(buf);
            return NULL;
        }
        buf = g;
        if (!xbox_hdd_read(v->h, cluster_at(v, c), buf + have, v->cluster_size)) {
            free(buf);
            return NULL;
        }
        have += v->cluster_size;
        c = next_cluster(v, c);
    }
    *len = have;
    return buf;
}

typedef struct {
    char     name[FATX_NAME_MAX + 1];
    int      dir;
    uint32_t first, size;
} Entry;

/* The live entries of a directory buffer. */
static int dir_entries(const uint8_t *d, size_t len, Entry *out, int max)
{
    size_t i;
    int n = 0;

    for (i = 0; i + FATX_ENTRY <= len && n < max; i += FATX_ENTRY) {
        uint8_t nl = d[i];
        if (nl == 0x00 || nl == 0xFF)
            break;                                   /* the end */
        if (nl == 0xE5 || nl > FATX_NAME_MAX)
            continue;                                /* deleted */
        memcpy(out[n].name, d + i + 2, nl);
        out[n].name[nl] = 0;
        out[n].dir = (d[i + 1] & FATX_ATTR_DIR) != 0;
        out[n].first = le32(d + i + 44);
        out[n].size = le32(d + i + 48);
        n++;
    }
    return n;
}

static int safe_name(const char *s)
{
    if (!s[0] || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (; *s; s++)
        if (*s == '/' || *s == '\\' || *s == ':' || (unsigned char)*s < 0x20)
            return 0;
    return 1;
}

static int copy_file(XboxFatx *v, const Entry *e, const char *host, char *err, size_t err_n)
{
    FILE *out = fopen(host, "wb");
    uint8_t *buf;
    uint32_t left = e->size, c = e->first, steps = 0;

    if (!out) {
        say(err, err_n, "cannot write %s: %s", host, strerror(errno));
        return 0;
    }
    buf = malloc(v->cluster_size);
    while (buf && left && c && steps++ < v->entries) {
        uint32_t take = left < v->cluster_size ? left : v->cluster_size;
        if (!xbox_hdd_read(v->h, cluster_at(v, c), buf, take) ||
            fwrite(buf, 1, take, out) != take) {
            say(err, err_n, "cannot copy %s", host);
            break;
        }
        left -= take;
        c = next_cluster(v, c);
    }
    free(buf);
    if (fclose(out) != 0 && !left)
        left = 1;
    if (left) {
        if (!err || !err[0])
            say(err, err_n, "%s ends early on the disk", host);
        remove(host);
        return 0;
    }
    return 1;
}

#define DIR_MAX 4096

static long extract_dir(XboxFatx *v, uint32_t first, const char *host, const char *guest,
                        int depth, int (*progress)(const char *, void *), void *ctx,
                        char *err, size_t err_n)
{
    size_t len;
    uint8_t *d;
    Entry *ents;
    int n, i;
    long files = 0;

    if (depth > 32) {
        say(err, err_n, "folders nested too deep at %s", guest);
        return -1;
    }
    make_dir(host);
    d = read_dir(v, first, &len);
    ents = malloc(DIR_MAX * sizeof *ents);
    if (!d || !ents) {
        free(d);
        free(ents);
        say(err, err_n, "cannot read the folder %s", guest[0] ? guest : "/");
        return -1;
    }
    n = dir_entries(d, len, ents, DIR_MAX);
    free(d);
    for (i = 0; i < n; i++) {
        char hp[1200], gp[600];
        if (!safe_name(ents[i].name)) {
            say(err, err_n, "a name on the disk is not safe to copy: \"%s\"", ents[i].name);
            free(ents);
            return -1;
        }
        snprintf(hp, sizeof hp, "%s%c%s", host, SEP, ents[i].name);
        snprintf(gp, sizeof gp, "%s%s%s", guest, guest[0] ? "/" : "", ents[i].name);
        if (progress && progress(gp, ctx)) {
            say(err, err_n, "stopped");
            free(ents);
            return -1;
        }
        if (ents[i].dir) {
            long k = extract_dir(v, ents[i].first, hp, gp, depth + 1, progress, ctx, err, err_n);
            if (k < 0) {
                free(ents);
                return -1;
            }
            files += k;
        } else {
            if (!copy_file(v, &ents[i], hp, err, err_n)) {
                free(ents);
                return -1;
            }
            files++;
        }
    }
    free(ents);
    return files;
}

long xbox_fatx_extract(XboxFatx *v, const char *dest,
                       int (*progress)(const char *path, void *ctx), void *ctx,
                       char *err, size_t err_n)
{
    if (err && err_n)
        err[0] = 0;
    return extract_dir(v, v->root, dest, "", 0, progress, ctx, err, err_n);
}

static int name_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return a[n] == 0;
}

uint8_t *xbox_fatx_read_file(XboxFatx *v, const char *path, uint32_t *size)
{
    uint32_t dir = v->root;
    const char *p = path;

    for (;;) {
        const char *slash = strchr(p, '/');
        size_t part = slash ? (size_t)(slash - p) : strlen(p), len;
        uint8_t *d = read_dir(v, dir, &len);
        Entry *ents = malloc(DIR_MAX * sizeof *ents);
        int n, i, found = -1;

        if (!d || !ents) {
            free(d);
            free(ents);
            return NULL;
        }
        n = dir_entries(d, len, ents, DIR_MAX);
        free(d);
        for (i = 0; i < n && found < 0; i++)
            if (name_eq(ents[i].name, p, part) && (slash ? ents[i].dir : !ents[i].dir))
                found = i;
        if (found < 0) {
            free(ents);
            return NULL;
        }
        if (slash) {
            dir = ents[found].first;
            p = slash + 1;
            free(ents);
            continue;
        }
        {
            Entry e = ents[found];
            uint8_t *buf = malloc(e.size ? e.size : 1);
            uint32_t got = 0, c = e.first, steps = 0;
            free(ents);
            while (buf && got < e.size && c && steps++ < v->entries) {
                uint32_t take = e.size - got < v->cluster_size ? e.size - got : v->cluster_size;
                if (!xbox_hdd_read(v->h, cluster_at(v, c), buf + got, take)) {
                    free(buf);
                    return NULL;
                }
                got += take;
                c = next_cluster(v, c);
            }
            if (!buf || got != e.size) {
                free(buf);
                return NULL;
            }
            *size = e.size;
            return buf;
        }
    }
}

/* ---- SHA-1 -------------------------------------------------------------- */

typedef struct { uint32_t h[5]; uint64_t n; uint8_t buf[64]; size_t used; } Sha1;

static uint32_t rol(uint32_t x, int k) { return x << k | x >> (32 - k); }

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    static const uint32_t h0[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    memcpy(s->h, h0, sizeof h0);
    s->n = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const uint8_t *p, size_t len)
{
    s->n += len;
    while (len) {
        size_t take = 64 - s->used < len ? 64 - s->used : len;
        memcpy(s->buf + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->n * 8u;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (i = 0; i < 20; i++)
        out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

void xbox_hdd_sha1(const void *data, size_t len, uint8_t out[20])
{
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

int xbox_hdd_sha1_file(const char *path, uint8_t out[20], uint64_t *size)
{
    uint8_t buf[65536];
    size_t got;
    Sha1 s;
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    sha1_init(&s);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        sha1_update(&s, buf, got);
    fclose(f);
    if (size)
        *size = s.n;
    sha1_final(&s, out);
    return 1;
}
