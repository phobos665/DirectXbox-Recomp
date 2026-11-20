/*
 * src/hdd/xbox_hdd.c on images made here: a FATX volume laid out by the
 * format's rules, raw and as qcow2 v3, extracted and read back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xbox_hdd.h"

#ifdef _WIN32
#include <direct.h>
#define SEP "\\"
static void make_dir(const char *p) { _mkdir(p); }
#else
#include <sys/stat.h>
#define SEP "/"
static void make_dir(const char *p) { mkdir(p, 0755); }
#endif

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CS        16384u               /* 32 sectors a cluster, as C: has */
#define VOL_SIZE  (8u << 20)           /* 513 table entries: FAT16, one 4 KB page */
#define DATA_AT   (0x1000u + 0x1000u)

static uint8_t *g_vol;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint8_t *cluster(uint32_t c) { return g_vol + DATA_AT + (c - 1) * CS; }

static void entry(uint8_t *at, const char *name, int dir, uint32_t first, uint32_t size)
{
    at[0] = (uint8_t)strlen(name);
    at[1] = dir ? 0x10 : 0;
    memset(at + 2, 0xFF, 42);
    memcpy(at + 2, name, strlen(name));
    put32(at + 44, first);
    put32(at + 48, size);
}

static uint8_t content(uint32_t i) { return (uint8_t)(i * 7u + 3u); }

/* root (1): xodash/ (2), XBOXDASH.XBE (3,4,6: 40000 bytes, a gap at 5),
 * a deleted entry, end. xodash/: update.xbe (7, 100 bytes). */
static void make_volume(void)
{
    uint8_t *fat;
    uint32_t i;

    g_vol = calloc(1, VOL_SIZE);
    memcpy(g_vol, "FATX", 4);
    put32(g_vol + 4, 0x12345678);
    put32(g_vol + 8, CS / 512);
    put32(g_vol + 12, 1);
    fat = g_vol + 0x1000;
    put16(fat + 0, 0xFFF8);                          /* the reserved entry */
    put16(fat + 2 * 1, 0xFFFF);                      /* root: one cluster */
    put16(fat + 2 * 2, 0xFFFF);                      /* xodash/ */
    put16(fat + 2 * 3, 4);
    put16(fat + 2 * 4, 6);
    put16(fat + 2 * 6, 0xFFFF);
    put16(fat + 2 * 7, 0xFFFF);
    memset(cluster(1), 0xFF, CS);
    entry(cluster(1) + 0, "xodash", 1, 2, 0);
    entry(cluster(1) + 64, "XBOXDASH.XBE", 0, 3, 40000);
    entry(cluster(1) + 128, "gone.bin", 0, 9, 10);
    cluster(1)[128] = 0xE5;
    memset(cluster(2), 0xFF, CS);
    entry(cluster(2), "update.xbe", 0, 7, 100);
    for (i = 0; i < 40000; i++) {
        uint32_t c = i / CS == 0 ? 3 : i / CS == 1 ? 4 : 6;
        cluster(c)[i % CS] = content(i);
    }
    memset(cluster(7), 0x5A, 100);
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    fwrite(p, 1, n, f);
    fclose(f);
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }

/* The volume as a qcow2 v3 disk with 64 KB clusters, the volume at disk
 * offset `at`: the header in cluster 0, L1 in 1, one L2 in 2, data after.
 * An all-zero data cluster is left unallocated, as qemu-img leaves it. */
static void write_qcow2(const char *path, uint64_t at)
{
    const uint32_t bits = 16, qcs = 1u << bits;
    uint64_t disk = at + VOL_SIZE, nclusters = disk / qcs, i, next = 3;
    uint8_t *img = calloc(1, (size_t)(3 + nclusters) * qcs);

    memcpy(img, "QFI\xfb", 4);
    be32(img + 4, 3);
    be32(img + 20, bits);
    be64(img + 24, disk);
    be32(img + 36, 1);                               /* one L1 entry covers 512 MB */
    be64(img + 40, qcs);
    be32(img + 96, 4);
    be32(img + 100, 104);
    be64(img + qcs, (2ull * qcs) | (1ull << 63));
    for (i = 0; i < nclusters; i++) {
        uint64_t off = i * qcs;
        const uint8_t *src = off >= at ? g_vol + (off - at) : NULL;
        uint64_t k, nz = 0;
        if (src)
            for (k = 0; k < qcs && !nz; k++)
                nz = src[k];
        if (!nz)
            continue;
        memcpy(img + next * qcs, src, qcs);
        be64(img + 2 * qcs + 8 * i, (next * qcs) | (1ull << 63));
        next++;
    }
    write_file(path, img, (size_t)next * qcs);
    free(img);
}

static void check_extract(const char *image, uint64_t at, const char *dest)
{
    char err[200], path[600];
    XboxHdd *h = xbox_hdd_open(image, err, sizeof err);
    XboxFatx *v;
    uint8_t *buf;
    uint32_t size = 0, i;
    int same = 1;
    FILE *f;
    long n;

    CHECK(h != NULL, "open %s: %s", image, err);
    if (!h)
        return;
    v = xbox_fatx_open(h, at, VOL_SIZE, err, sizeof err);
    CHECK(v != NULL, "FATX at 0x%llx: %s", (unsigned long long)at, err);
    if (!v) {
        xbox_hdd_close(h);
        return;
    }
    n = xbox_fatx_extract(v, dest, NULL, NULL, err, sizeof err);
    CHECK(n == 2, "files copied: %ld (%s)", n, err);

    snprintf(path, sizeof path, "%s" SEP "XBOXDASH.XBE", dest);
    f = fopen(path, "rb");
    buf = malloc(40001);
    CHECK(f && fread(buf, 1, 40001, f) == 40000, "XBOXDASH.XBE is 40000 bytes");
    for (i = 0; f && i < 40000; i++)
        same &= buf[i] == content(i);
    CHECK(same, "XBOXDASH.XBE's bytes, across a gap in its chain");
    if (f)
        fclose(f);
    free(buf);
    snprintf(path, sizeof path, "%s" SEP "xodash" SEP "update.xbe", dest);
    f = fopen(path, "rb");
    CHECK(f != NULL, "xodash/update.xbe copied");
    if (f)
        fclose(f);
    snprintf(path, sizeof path, "%s" SEP "gone.bin", dest);
    f = fopen(path, "rb");
    CHECK(f == NULL, "a deleted entry is not copied");
    if (f)
        fclose(f);

    buf = xbox_fatx_read_file(v, "XODASH/Update.xbe", &size);
    CHECK(buf && size == 100 && buf[0] == 0x5A && buf[99] == 0x5A, "read_file, any case");
    free(buf);
    CHECK(xbox_fatx_read_file(v, "xodash/nothere.xbe", &size) == NULL, "a missing file");
    xbox_fatx_close(v);
    xbox_hdd_close(h);
}

int main(int argc, char **argv)
{
    char dir[512], path[600], out[600], err[200];
    XboxHdd *h;

    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "xbox_hdd_test");
    make_dir(dir);
    make_volume();

    snprintf(path, sizeof path, "%s" SEP "c.img", dir);
    write_file(path, g_vol, VOL_SIZE);
    snprintf(out, sizeof out, "%s" SEP "raw_out", dir);
    check_extract(path, 0, out);

    /* qcow2, with the volume 1 MB into the disk as C: is 2.2 GB in. */
    snprintf(path, sizeof path, "%s" SEP "disk.qcow2", dir);
    write_qcow2(path, 1u << 20);
    h = xbox_hdd_open(path, err, sizeof err);
    CHECK(h && xbox_hdd_is_qcow2(h) && xbox_hdd_size(h) == (1u << 20) + VOL_SIZE,
          "qcow2 recognised, its disk size");
    if (h) {
        uint8_t z[16];
        memset(z, 1, sizeof z);
        CHECK(xbox_hdd_read(h, 1000, z, sizeof z) && !z[0] && !z[15], "unallocated reads zero");
        xbox_hdd_close(h);
    }
    snprintf(out, sizeof out, "%s" SEP "qcow2_out", dir);
    check_extract(path, 1u << 20, out);

    /* Not a FATX volume where one is asked for. */
    h = xbox_hdd_open(path, err, sizeof err);
    CHECK(h && !xbox_fatx_open(h, 0, VOL_SIZE, err, sizeof err) && strstr(err, "no FATX"),
          "no volume: '%s'", err);
    xbox_hdd_close(h);

    {   /* SHA-1's known answers (FIPS 180 examples). */
        uint8_t d[20];
        static const uint8_t abc[20] = { 0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                         0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
        static const uint8_t two[20] = { 0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,0xba,0xae,
                                         0x4a,0xa1,0xf9,0x51,0x29,0xe5,0xe5,0x46,0x70,0xf1 };
        xbox_hdd_sha1("abc", 3, d);
        CHECK(!memcmp(d, abc, 20), "SHA-1 of abc");
        xbox_hdd_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
        CHECK(!memcmp(d, two, 20), "SHA-1 of the two-block example");
    }

    free(g_vol);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("xbox_hdd: all checks passed\n");
    return 0;
}
