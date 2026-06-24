/* <unistd.h> for the BoltOS DOOM port (dg_libc.c). */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

int     access(const char *path, int mode);
int     unlink(const char *path);
int     close(int fd);
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
long    lseek(int fd, long off, int whence);
int     isatty(int fd);
int     usleep(unsigned usec);
unsigned sleep(unsigned sec);
char   *getcwd(char *buf, size_t size);
