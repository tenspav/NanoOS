/* GCC may lower aggregate assignment/initialization to these in freestanding
 * code. Byte operations also work for unaligned source/destination addresses.
 * No allocation, libc, cache maintenance or device access. */
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < n; ++i) d[i] = (unsigned char)value;
    return dst;
}
