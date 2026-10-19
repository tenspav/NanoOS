#include "asset.h"
extern const uint8_t __asset_base[], __asset_max[];
struct npak_entry { char name[32]; uint32_t off, size, padded, crc, sector, rsv[3]; };

uint32_t crc32(const void *data, uint32_t n) {
    const uint8_t *p = data;
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

const void *asset_find(const char *name, uint32_t *size) {
    const uint32_t *h = (const uint32_t *)__asset_base;
    uint32_t max = (uint32_t)__asset_max;
    if (h[0] != 0x4b41504eu || h[1] != 1 || h[2] > 64) return 0;
    const struct npak_entry *e = (const struct npak_entry *)(h + 4);
    for (uint32_t i = 0; i < h[2]; ++i, ++e) {
        uint32_t j = 0;
        while (j < 31 && name[j] && name[j] == e->name[j]) ++j;
        if (name[j] || e->name[j]) continue;
        if (e->off > max || e->size > max - e->off) return 0;
        const uint8_t *p = __asset_base + e->off;
        if (crc32(p, e->size) != e->crc) return 0;
        *size = e->size;
        return p;
    }
    return 0;
}
