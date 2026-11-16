#ifndef SHMUP_NANO_STDLIB_H
#define SHMUP_NANO_STDLIB_H
#include <stddef.h>
#define RAND_MAX 0x7fff
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
int atoi(const char *s);
double atof(const char *s);
double strtod(const char *nptr, char **endptr);
int abs(int j);
int rand(void);
void srand(unsigned int seed);
void exit(int status);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
#endif
