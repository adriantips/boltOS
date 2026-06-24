#pragma once
#include <stdint.h>
/* ===========================================================================
 *  BoltOS multi-user accounts. A tiny /etc/passwd-style table persisted to the
 *  filesystem. Passwords are stored as a 64-bit hash (not crypto-grade - this
 *  is a hobby OS - but it keeps plaintext off disk). One "current user" is the
 *  identity the shell and GUI act as after login.
 * ===========================================================================*/
#define USER_MAX      16
#define USER_NAME_MAX 24

typedef struct { char name[USER_NAME_MAX]; uint64_t hash; int uid; } user_t;

void        users_init(void);                       /* load /etc/passwd or seed defaults */
int         users_auth(const char *name, const char *pw);  /* uid on success, -1 on fail */
int         users_add(const char *name, const char *pw, int uid);  /* 0 ok, -1 fail */
int         users_set_password(const char *name, const char *pw);
int         users_count(void);
const user_t *users_get(int i);

void        users_login(int uid);                   /* set the current identity */
void        users_logout(void);
const char *users_current(void);                    /* current username, "" if none */
int         users_current_uid(void);                /* uid, or -1 if logged out */
int         users_is_root(void);
