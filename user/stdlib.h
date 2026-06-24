#pragma once
/* BoltOS userland <stdlib.h>. */
#include <stddef.h>

void *malloc(unsigned long);
void *calloc(unsigned long, unsigned long);
void *realloc(void *, unsigned long);
void  free(void *);

long  strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
double strtod(const char *, char **);
double atof(const char *);
int    atoi(const char *);
long   atol(const char *);
int    abs(int);
long   labs(long);

void   srand(unsigned);
int    rand(void);
#define RAND_MAX 0x7fffffff

void   qsort(void *, unsigned long, unsigned long, int (*)(const void *, const void *));
void  *bsearch(const void *, const void *, unsigned long, unsigned long, int (*)(const void *, const void *));

void   exit(int) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
char  *getenv(const char *);
