#pragma once
#include <stdarg.h>
/* BoltOS userland <stdio.h>: a thin buffered FILE layer over fd syscalls. */

typedef struct { int fd; int eof; int used; int _r; } FILE;
extern FILE *stdin, *stdout, *stderr;

#define EOF (-1)

int   printf(const char *, ...);
int   vprintf(const char *, va_list);
int   fprintf(int fd, const char *, ...);       /* fd-based fast path */
int   snprintf(char *, unsigned long, const char *, ...);
int   vsnprintf(char *, unsigned long, const char *, va_list);
int   sprintf(char *, const char *, ...);
int   putchar(int);
int   puts(const char *);

FILE *fopen(const char *, const char *);
int   fclose(FILE *);
unsigned long fread(void *, unsigned long, unsigned long, FILE *);
unsigned long fwrite(const void *, unsigned long, unsigned long, FILE *);
int   fputc(int, FILE *);
int   fputs(const char *, FILE *);
int   fgetc(FILE *);
char *fgets(char *, int, FILE *);
int   fflush(FILE *);
int   feof(FILE *);
long  ftell(FILE *);
int   fseek(FILE *, long, int);
