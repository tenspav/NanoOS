#ifndef NANOOS_SDK_FS_H
#define NANOOS_SDK_FS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nano_file nano_file_t;

/* Mount the iPod NAND HFS+ volume (idempotent). Returns 0 on success or < 0 on error. */
int nano_fs_mount(void);

/* Open a read-only file by HFS+ path (e.g., "/Apps/Data/Shmup/data/config.cfg"). */
nano_file_t *nano_fs_open(const char *path);

/* Return file size in bytes. */
uint64_t nano_fs_size(const nano_file_t *f);

/* Read up to `len` bytes from current cursor; returns bytes read or < 0 on error. */
int nano_fs_read(nano_file_t *f, void *buf, uint32_t len);

/* Positional read at byte offset `off`; returns bytes read or < 0 on error. */
int nano_fs_pread(nano_file_t *f, uint64_t off, void *buf, uint32_t len);

/* Seek cursor (whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END). Returns 0 or -1. */
int nano_fs_seek(nano_file_t *f, int64_t offset, int whence);

/* Return current byte offset. */
uint64_t nano_fs_tell(const nano_file_t *f);

/* Close file handle and release resources. */
void nano_fs_close(nano_file_t *f);

/* Convenience: read entire file into a malloc'd buffer (+1 NUL byte). Caller frees. */
void *nano_fs_read_all(const char *path, uint32_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* NANOOS_SDK_FS_H */
