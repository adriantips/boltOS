/* ===========================================================================
 *  BoltOS  -  kernel/users.c
 *  Multi-user account table, persisted to /etc/passwd as "name:hexhash:uid"
 *  lines. The FS autosaves, so accounts survive reboots.
 * ===========================================================================*/
#include <stdint.h>
#include "users.h"
#include "fs.h"
#include "string.h"
#include "kprintf.h"

static user_t users[USER_MAX];
static int    nuser;
static int    cur_uid = -1;
static char   cur_name[USER_NAME_MAX];

/* FNV-1a 64-bit over the password, salted with the username so identical
 * passwords for different users don't share a hash. */
static uint64_t pw_hash(const char *name, const char *pw) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    h ^= 0x3a; h *= 1099511628211ull;
    for (const char *p = pw;   *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    return h;
}

static user_t *find(const char *name) {
    for (int i = 0; i < nuser; i++)
        if (strcmp(users[i].name, name) == 0) return &users[i];
    return 0;
}

/* ---- /etc/passwd serialisation ----------------------------------------- */
static void to_hex(uint64_t v, char *out) {
    const char *d = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = d[(v >> ((15 - i) * 4)) & 0xF];
    out[16] = 0;
}
static uint64_t from_hex(const char *s) {
    uint64_t v = 0;
    for (int i = 0; i < 16 && s[i]; i++) {
        char c = s[i]; int d = (c >= '0' && c <= '9') ? c - '0'
                              : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

static void uid_str(int v, char *out) {
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[12]; int t = 0; int neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
    int k = 0; if (neg) out[k++] = '-';
    while (t) out[k++] = tmp[--t];
    out[k] = 0;
}

static void save(void) {
    char buf[USER_MAX * 64]; int n = 0;
    for (int i = 0; i < nuser; i++) {
        char hex[20]; to_hex(users[i].hash, hex);
        char num[12]; uid_str(users[i].uid, num);
        for (const char *p = users[i].name; *p; p++) buf[n++] = *p;
        buf[n++] = ':';
        for (const char *p = hex; *p; p++) buf[n++] = *p;
        buf[n++] = ':';
        for (const char *p = num; *p; p++) buf[n++] = *p;
        buf[n++] = '\n';
    }
    buf[n] = 0;
    fs_node *e = fs_lookup("/etc"); if (!e) fs_create("/etc", 1);
    fs_node *f = fs_lookup("/etc/passwd"); if (!f) f = fs_create("/etc/passwd", 0);
    if (f) fs_write(f, buf, (uint32_t)n);
}

static int load(void) {
    fs_node *f = fs_lookup("/etc/passwd");
    if (!f || !f->data || f->size == 0) return -1;
    nuser = 0;
    const char *s = (const char *)f->data;
    uint32_t i = 0;
    while (i < f->size && nuser < USER_MAX) {
        char line[80]; int ln = 0;
        while (i < f->size && s[i] != '\n' && ln < 79) line[ln++] = s[i++];
        if (i < f->size && s[i] == '\n') i++;
        line[ln] = 0;
        if (ln == 0) continue;
        /* split name:hash:uid */
        char name[USER_NAME_MAX]; int k = 0;
        int j = 0;
        while (line[j] && line[j] != ':' && k < USER_NAME_MAX - 1) name[k++] = line[j++];
        name[k] = 0;
        if (line[j] != ':') continue; j++;
        char hex[20]; int hk = 0;
        while (line[j] && line[j] != ':' && hk < 19) hex[hk++] = line[j++]; hex[hk] = 0;
        if (line[j] != ':') continue; j++;
        int uid = atoi(&line[j]);
        strncpy(users[nuser].name, name, USER_NAME_MAX);
        users[nuser].hash = from_hex(hex);
        users[nuser].uid  = uid;
        nuser++;
    }
    return nuser > 0 ? 0 : -1;
}

void users_init(void) {
    if (load() == 0) return;            /* restored from disk */
    nuser = 0;
    users_add("user", "bolt", 1000);    /* default desktop account */
    users_add("root", "root", 0);       /* superuser */
    save();
}

int users_add(const char *name, const char *pw, int uid) {
    if (nuser >= USER_MAX || !name[0]) return -1;
    if (find(name)) return -1;
    strncpy(users[nuser].name, name, USER_NAME_MAX);
    users[nuser].hash = pw_hash(name, pw);
    users[nuser].uid  = uid;
    nuser++;
    save();
    return 0;
}

int users_set_password(const char *name, const char *pw) {
    user_t *u = find(name); if (!u) return -1;
    u->hash = pw_hash(name, pw); save(); return 0;
}

int users_auth(const char *name, const char *pw) {
    user_t *u = find(name);
    if (!u) return -1;
    return (u->hash == pw_hash(name, pw)) ? u->uid : -1;
}

int           users_count(void)      { return nuser; }
const user_t *users_get(int i)       { return (i >= 0 && i < nuser) ? &users[i] : 0; }

void users_login(int uid) {
    for (int i = 0; i < nuser; i++)
        if (users[i].uid == uid) { cur_uid = uid; strncpy(cur_name, users[i].name, sizeof(cur_name)); return; }
}
void        users_logout(void)       { cur_uid = -1; cur_name[0] = 0; }
const char *users_current(void)      { return cur_uid >= 0 ? cur_name : ""; }
int         users_current_uid(void)  { return cur_uid; }
int         users_is_root(void)      { return cur_uid == 0; }
