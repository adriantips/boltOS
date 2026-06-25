#pragma once
#include <stdint.h>
#include <stddef.h>
/* <sys/types.h> -- minimal POSIX type set for ported libraries (NetSurf). */
typedef long           ssize_t;
typedef long           off_t;
typedef int64_t        off64_t;
typedef unsigned long  ino_t;
typedef unsigned int   mode_t;
typedef int            pid_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef long           time_t;
typedef unsigned long  dev_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef unsigned long  nlink_t;
typedef unsigned long  fsblkcnt_t;
typedef int64_t        intmax_t;
typedef uint64_t       uintmax_t;
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
