#include "nano_fs.h"
#include "nano_app.h"
#include "memmap.h"
#include "storage/nano_game.h"
#include <stdlib.h>

/* Place ns_game (~1.5 MiB) in MM_GAME_HEAP, clear of report mailbox/mirrors. */
static struct ns_game *const g_ns_game = (struct ns_game *)(uintptr_t)(MM_GAME_HEAP + 0x00400000u);
static int g_mounted = 0;

struct nano_file {
    struct ns_file file;
    uint64_t pos;
};

int nano_fs_mount(void) {
    if (g_mounted) return 0;
    const struct nanoos_api *api = nano_app_api();
    if (!api) return -1;
    int rc = ns_nano_game_mount(g_ns_game, api);
    if (rc == 0) g_mounted = 1;
    return rc;
}

nano_file_t *nano_fs_open(const char *path) {
    if (!path || !g_mounted) return 0;
    nano_file_t *f = (nano_file_t *)calloc(1, sizeof(nano_file_t));
    if (!f) return 0;
    if (ns_game_open(g_ns_game, path, &f->file) != 0) {
        free(f);
        return 0;
    }
    f->pos = 0;
    return f;
}

uint64_t nano_fs_size(const nano_file_t *f) {
    return f ? f->file.fork.size : 0u;
}

int nano_fs_pread(nano_file_t *f, uint64_t off, void *buf, uint32_t len) {
    if (!f || !buf || !len || !g_mounted) return -1;
    if (off >= f->file.fork.size) return 0;
    uint64_t avail = f->file.fork.size - off;
    uint32_t want = (uint64_t)len > avail ? (uint32_t)avail : len;
    uint32_t got = 0;
    if (ns_game_read(g_ns_game, &f->file, off, buf, want, &got) != 0) return -1;
    return (int)got;
}

int nano_fs_read(nano_file_t *f, void *buf, uint32_t len) {
    int got = nano_fs_pread(f, f->pos, buf, len);
    if (got > 0) f->pos += (uint32_t)got;
    return got;
}

int nano_fs_seek(nano_file_t *f, int64_t offset, int whence) {
    if (!f) return -1;
    int64_t base = 0;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->pos;
    else if (whence == 2) base = (int64_t)f->file.fork.size;
    else return -1;
    int64_t next = base + offset;
    if (next < 0) next = 0;
    if ((uint64_t)next > f->file.fork.size) next = (int64_t)f->file.fork.size;
    f->pos = (uint64_t)next;
    return 0;
}

uint64_t nano_fs_tell(const nano_file_t *f) {
    return f ? f->pos : 0u;
}

void nano_fs_close(nano_file_t *f) {
    free(f);
}

void *nano_fs_read_all(const char *path, uint32_t *out_size) {
    if (out_size) *out_size = 0;
    nano_file_t *f = nano_fs_open(path);
    if (!f) return 0;
    uint64_t sz64 = nano_fs_size(f);
    if (sz64 > 0x01000000u) { nano_fs_close(f); return 0; }
    uint32_t sz = (uint32_t)sz64;
    uint8_t *buf = (uint8_t *)malloc(sz + 1u);
    if (!buf) { nano_fs_close(f); return 0; }
    uint32_t off = 0;
    while (off < sz) {
        int r = nano_fs_read(f, buf + off, sz - off);
        if (r <= 0) break;
        off += (uint32_t)r;
    }
    nano_fs_close(f);
    if (off != sz) { free(buf); return 0; }
    buf[sz] = 0;
    if (out_size) *out_size = sz;
    return buf;
}
