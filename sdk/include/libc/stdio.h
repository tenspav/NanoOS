#ifndef SHMUP_NANO_STDIO_H
#define SHMUP_NANO_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct { int dummy; } FILE;
extern FILE *stdout;
int fflush(FILE *stream);
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
#endif
