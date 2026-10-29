/**
 * An Xbox hard disk image, read: xemu's (qcow2) or a raw one, and the FATX
 * volumes on it, so a player's own dashboard can be copied off the C: drive
 * of an image they already have (docs/dashboard-files.md).
 *
 * Read-only. qcow2 versions 2 and 3, uncompressed and unencrypted, with no
 * backing file -- what xemu writes. Written from the formats' descriptions
 * (QEMU's docs/interop/qcow2.txt; FATX as XboxDev documents it), with no
 * code of either project's.
 */
#ifndef XBOX_HDD_H
#define XBOX_HDD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The retail partition layout every Xbox disk has. */
#define XBOX_HDD_C_OFFSET 0x8CA80000ull
#define XBOX_HDD_C_SIZE   0x1F400000ull
#define XBOX_HDD_E_OFFSET 0xABE80000ull
#define XBOX_HDD_E_SIZE   0x1312D6000ull

typedef struct XboxHdd XboxHdd;

/* A disk image: qcow2 by its magic, else raw. NULL with err filled. */
XboxHdd *xbox_hdd_open(const char *path, char *err, size_t err_n);
uint64_t xbox_hdd_size(const XboxHdd *h);
/* n bytes at off; 1 when read, 0 with xbox_hdd_error saying why. Past the
 * end, or unallocated, reads as zeros. */
int xbox_hdd_read(XboxHdd *h, uint64_t off, void *buf, size_t n);
const char *xbox_hdd_error(const XboxHdd *h);
int xbox_hdd_is_qcow2(const XboxHdd *h);
void xbox_hdd_close(XboxHdd *h);

/* A FATX volume at [offset, offset + size) of the disk. */
typedef struct XboxFatx XboxFatx;
XboxFatx *xbox_fatx_open(XboxHdd *h, uint64_t offset, uint64_t size, char *err, size_t err_n);
void xbox_fatx_close(XboxFatx *v);

/* Copy every file and folder under the volume's root into dest (made if
 * missing). progress, when given, is called with each file's path as it is
 * copied and may return non-zero to stop. Returns the number of files
 * copied, or -1 with err filled. A name that could escape dest (".", "..",
 * a separator) is refused. */
long xbox_fatx_extract(XboxFatx *v, const char *dest,
                       int (*progress)(const char *path, void *ctx), void *ctx,
                       char *err, size_t err_n);

/* A file's whole contents by its path from the root ("xodash/update.xbe"),
 * case-insensitively. malloc'd; NULL when it is not there. */
uint8_t *xbox_fatx_read_file(XboxFatx *v, const char *path, uint32_t *size);

/* SHA-1 (FIPS 180-4), for telling a file is the one expected. */
void xbox_hdd_sha1(const void *data, size_t len, uint8_t out[20]);
/* The same of a host file; 1 when it was read. */
int xbox_hdd_sha1_file(const char *path, uint8_t out[20], uint64_t *size);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_HDD_H */
