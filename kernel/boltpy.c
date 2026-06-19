/* ===========================================================================
 *  BoltPython  -  kernel/boltpy.c
 *  A freestanding, integer-only Python-3 subset interpreter for BoltOS.
 *
 *  Pipeline:  source -> lexer (INDENT/DEDENT aware) -> recursive-descent parser
 *             -> AST -> tree-walking evaluator.  All interpreter memory comes
 *             from an arena backed by kmalloc(); the whole arena is released
 *             when a program (or REPL session) ends, so there is no GC.
 *
 *  No floating point (the kernel disables SSE/x87): ints are 64-bit and `/`
 *  behaves like floor division `//`.  See include/boltpy.h for the surface.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>
#include "boltpy.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "keyboard.h"

#define BPY_VERSION "BoltPython 1.0 (integer-only Python 3 subset)"
#define MAX_DEPTH   180          /* call-recursion guard (kernel stack is small) */
#define ARENA_BLK   (96 * 1024)
#define RANGE_CAP   200000       /* materialised range()/list() element limit    */

/* ---------------------------------------------------------------------------
 *  Arena allocator
 * ------------------------------------------------------------------------- */
typedef struct ablk { struct ablk *next; uint32_t used, cap; } ablk;

struct bpy_vm;
static uint8_t emergency[256];    /* returned on OOM so callers don't deref 0   */

/* ---------------------------------------------------------------------------
 *  Values
 * ------------------------------------------------------------------------- */
typedef struct Str  { uint32_t len; char *b; } Str;
typedef struct Value Value;
typedef struct List { uint32_t len, cap; Value *items; } List;
typedef struct Node Node;
typedef struct Func { const char *name; char **params; int nparams; Node *body; } Func;

enum { VT_NONE, VT_INT, VT_BOOL, VT_STR, VT_LIST, VT_FUNC, VT_BUILTIN };

struct Value {
    uint8_t t;
    union { int64_t i; Str *s; List *l; Func *fn; int bi; } u;
};

/* ---------------------------------------------------------------------------
 *  Scopes
 * ------------------------------------------------------------------------- */
typedef struct Scope {
    char **names; Value *vals; int n, cap;
    char **gdecl; int ngdecl;
} Scope;

/* ---------------------------------------------------------------------------
 *  VM
 * ------------------------------------------------------------------------- */
enum { F_NONE, F_RET, F_BREAK, F_CONT };

struct bpy_vm {
    ablk  *arena;
    int    error;
    char   errmsg[192];
    Scope  globals;
    Scope *cur;
    int    flow;
    Value  retval;
    int    depth;
    int    (*input)(char *, int);
    Value  last;            /* last top-level expression value (REPL echo)      */
    int    last_is_expr;
};

/* ---------------------------------------------------------------------------
 *  AST
 * ------------------------------------------------------------------------- */
enum {
    ND_NUM, ND_STR, ND_NAME, ND_TRUE, ND_FALSE, ND_NONE, ND_LIST,
    ND_UNARY, ND_BIN, ND_BOOL, ND_NOT, ND_CALL, ND_INDEX, ND_SLICE, ND_ATTR,
    ND_TERNARY,
    ND_EXPRSTMT, ND_ASSIGN, ND_AUG, ND_IF, ND_WHILE, ND_FOR, ND_DEF, ND_RETURN,
    ND_BREAK, ND_CONTINUE, ND_PASS, ND_BLOCK, ND_GLOBAL
};

struct Node {
    uint8_t  k;
    const char *s;
    int64_t  num;
    Node    *a, *b, *c, *d;
    Node   **kids; int nk;
    char   **names; int nn;
    char   **kw; Node **kwv; int nkw;
    int      line;
};

/* ---------------------------------------------------------------------------
 *  forward declarations
 * ------------------------------------------------------------------------- */
static void  *aalloc(bpy_vm *, uint32_t);
static Value  eval(bpy_vm *, Node *);
static void   exec(bpy_vm *, Node *);
static void   exec_block(bpy_vm *, Node *);
static Value  call_value(bpy_vm *, Value, Value *, int, char **, Value *, int);
static Value  call_method(bpy_vm *, Value, const char *, Value *, int);
static int    builtin_index(const char *);
static Value  call_builtin(bpy_vm *, int, Value *, int, char **, Value *, int);

/* ---------------------------------------------------------------------------
 *  arena
 * ------------------------------------------------------------------------- */
static void *aalloc(bpy_vm *vm, uint32_t n) {
    n = (n + 7u) & ~7u;
    ablk *b = vm->arena;
    if (!b || b->used + n > b->cap) {
        uint32_t cap = n > ARENA_BLK ? n : ARENA_BLK;
        ablk *nb = (ablk *)kmalloc(sizeof(ablk) + cap);
        if (!nb) { if (!vm->error) { vm->error = 1; strcpy(vm->errmsg, "MemoryError: out of memory"); }
                   return emergency; }
        nb->cap = cap; nb->used = 0; nb->next = vm->arena; vm->arena = nb; b = nb;
    }
    uint8_t *p = (uint8_t *)(b + 1) + b->used;
    b->used += n;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
    return p;
}
static char *adup(bpy_vm *vm, const char *s, uint32_t len) {
    char *d = (char *)aalloc(vm, len + 1);
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    d[len] = 0; return d;
}
static char *adups(bpy_vm *vm, const char *s) { return adup(vm, s, (uint32_t)strlen(s)); }

/* ---------------------------------------------------------------------------
 *  errors
 * ------------------------------------------------------------------------- */
static void err(bpy_vm *vm, const char *msg) {
    if (vm->error) return;
    vm->error = 1;
    int i = 0; while (msg[i] && i < (int)sizeof(vm->errmsg) - 1) { vm->errmsg[i] = msg[i]; i++; }
    vm->errmsg[i] = 0;
}
static void err2(bpy_vm *vm, const char *a, const char *b) {
    if (vm->error) return;
    char buf[192]; int i = 0;
    for (const char *p = a; *p && i < 180; p++) buf[i++] = *p;
    for (const char *p = b; *p && i < 190; p++) buf[i++] = *p;
    buf[i] = 0; err(vm, buf);
}
static char *i64str(int64_t v, char *buf) {                 /* buf >= 24 */
    char tmp[24]; int i = 0; uint64_t u;
    int neg = v < 0;
    u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (!u) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + u % 10); u /= 10; }
    int j = 0; if (neg) buf[j++] = '-';
    while (i--) buf[j++] = tmp[i];
    buf[j] = 0; return buf;
}

/* ---------------------------------------------------------------------------
 *  value constructors / helpers
 * ------------------------------------------------------------------------- */
static Value v_none(void)        { Value v; v.t = VT_NONE; v.u.i = 0; return v; }
static Value v_int(int64_t i)    { Value v; v.t = VT_INT;  v.u.i = i; return v; }
static Value v_bool(int b)       { Value v; v.t = VT_BOOL; v.u.i = b ? 1 : 0; return v; }
static Value v_builtin(int idx)  { Value v; v.t = VT_BUILTIN; v.u.bi = idx; return v; }

static Value v_strn(bpy_vm *vm, const char *s, uint32_t len) {
    Str *st = (Str *)aalloc(vm, sizeof(Str));
    st->len = len; st->b = adup(vm, s, len);
    Value v; v.t = VT_STR; v.u.s = st; return v;
}
static Value v_str(bpy_vm *vm, const char *s) { return v_strn(vm, s, (uint32_t)strlen(s)); }

static List *list_new(bpy_vm *vm, uint32_t cap) {
    List *l = (List *)aalloc(vm, sizeof(List));
    l->len = 0; l->cap = cap ? cap : 4;
    l->items = (Value *)aalloc(vm, l->cap * sizeof(Value));
    return l;
}
static Value v_list(List *l) { Value v; v.t = VT_LIST; v.u.l = l; return v; }

static void list_push(bpy_vm *vm, List *l, Value x) {
    if (l->len >= l->cap) {
        uint32_t nc = l->cap * 2;
        Value *ni = (Value *)aalloc(vm, nc * sizeof(Value));
        for (uint32_t i = 0; i < l->len; i++) ni[i] = l->items[i];
        l->items = ni; l->cap = nc;
    }
    l->items[l->len++] = x;
}

static int truthy(Value v) {
    switch (v.t) {
    case VT_NONE: return 0;
    case VT_INT: case VT_BOOL: return v.u.i != 0;
    case VT_STR:  return v.u.s->len != 0;
    case VT_LIST: return v.u.l->len != 0;
    default: return 1;
    }
}
static const char *type_name(Value v) {
    switch (v.t) {
    case VT_NONE: return "NoneType";
    case VT_INT:  return "int";
    case VT_BOOL: return "bool";
    case VT_STR:  return "str";
    case VT_LIST: return "list";
    case VT_FUNC: return "function";
    default:      return "builtin_function_or_method";
    }
}

/* string-builder over the arena */
typedef struct { bpy_vm *vm; char *b; uint32_t len, cap; } SB;
static void sb_init(SB *s, bpy_vm *vm) { s->vm = vm; s->cap = 64; s->len = 0; s->b = (char *)aalloc(vm, s->cap); }
static void sb_putc(SB *s, char c) {
    if (s->len + 1 >= s->cap) {
        uint32_t nc = s->cap * 2; char *nb = (char *)aalloc(s->vm, nc);
        for (uint32_t i = 0; i < s->len; i++) nb[i] = s->b[i];
        s->b = nb; s->cap = nc;
    }
    s->b[s->len++] = c;
}
static void sb_puts(SB *s, const char *p) { while (*p) sb_putc(s, *p++); }
static void sb_putn(SB *s, const char *p, uint32_t n) { while (n--) sb_putc(s, *p++); }
static void sb_puti(SB *s, int64_t v) { char buf[24]; sb_puts(s, i64str(v, buf)); }

static void vfmt(bpy_vm *vm, SB *s, Value v, int repr) {
    char buf[24];
    switch (v.t) {
    case VT_NONE: sb_puts(s, "None"); break;
    case VT_BOOL: sb_puts(s, v.u.i ? "True" : "False"); break;
    case VT_INT:  sb_puts(s, i64str(v.u.i, buf)); break;
    case VT_STR:
        if (!repr) { sb_putn(s, v.u.s->b, v.u.s->len); break; }
        sb_putc(s, '\'');
        for (uint32_t i = 0; i < v.u.s->len; i++) {
            char c = v.u.s->b[i];
            if (c == '\\' || c == '\'') { sb_putc(s, '\\'); sb_putc(s, c); }
            else if (c == '\n') sb_puts(s, "\\n");
            else if (c == '\t') sb_puts(s, "\\t");
            else sb_putc(s, c);
        }
        sb_putc(s, '\'');
        break;
    case VT_LIST:
        sb_putc(s, '[');
        for (uint32_t i = 0; i < v.u.l->len; i++) {
            if (i) sb_puts(s, ", ");
            vfmt(vm, s, v.u.l->items[i], 1);
        }
        sb_putc(s, ']');
        break;
    case VT_FUNC:    sb_puts(s, "<function "); sb_puts(s, v.u.fn->name ? v.u.fn->name : "?"); sb_putc(s, '>'); break;
    default:         sb_puts(s, "<builtin>"); break;
    }
}
static Value to_str_value(bpy_vm *vm, Value v, int repr) {
    SB s; sb_init(&s, vm); vfmt(vm, &s, v, repr);
    return v_strn(vm, s.b, s.len);
}
static void print_value(bpy_vm *vm, Value v, int repr) {
    SB s; sb_init(&s, vm); vfmt(vm, &s, v, repr);
    for (uint32_t i = 0; i < s.len; i++) kputc(s.b[i]);
}

/* equality / ordering ----------------------------------------------------- */
static int str_cmp(Str *a, Str *b) {
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a->b[i], y = (unsigned char)b->b[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return a->len == b->len ? 0 : (a->len < b->len ? -1 : 1);
}
static int val_eq(Value a, Value b) {
    int an = (a.t == VT_INT || a.t == VT_BOOL), bn = (b.t == VT_INT || b.t == VT_BOOL);
    if (an && bn) return a.u.i == b.u.i;
    if (a.t != b.t) return 0;
    switch (a.t) {
    case VT_NONE: return 1;
    case VT_STR:  return a.u.s->len == b.u.s->len && str_cmp(a.u.s, b.u.s) == 0;
    case VT_LIST:
        if (a.u.l->len != b.u.l->len) return 0;
        for (uint32_t i = 0; i < a.u.l->len; i++) if (!val_eq(a.u.l->items[i], b.u.l->items[i])) return 0;
        return 1;
    default: return a.u.fn == b.u.fn;
    }
}
/* returns -1/0/1; sets *ok=0 if uncomparable */
static int val_cmp(Value a, Value b, int *ok) {
    *ok = 1;
    int an = (a.t == VT_INT || a.t == VT_BOOL), bn = (b.t == VT_INT || b.t == VT_BOOL);
    if (an && bn) return a.u.i < b.u.i ? -1 : (a.u.i > b.u.i ? 1 : 0);
    if (a.t == VT_STR && b.t == VT_STR) return str_cmp(a.u.s, b.u.s);
    *ok = 0; return 0;
}

/* ---------------------------------------------------------------------------
 *  Lexer
 * ------------------------------------------------------------------------- */
enum { TK_EOF, TK_NL, TK_INDENT, TK_DEDENT, TK_NAME, TK_NUM, TK_STR, TK_OP };
typedef struct { uint8_t k; const char *s; int64_t num; int line; } Tok;

typedef struct {
    bpy_vm *vm; const char *p; int line; int parens;
    int ind[80]; int nind;
    Tok *t; int n, cap;
    int atbol;
} Lex;

static void tok_add(Lex *L, uint8_t k, const char *s, int64_t num) {
    if (L->n >= L->cap) {
        int nc = L->cap ? L->cap * 2 : 64;
        Tok *nt = (Tok *)aalloc(L->vm, (uint32_t)nc * sizeof(Tok));
        for (int i = 0; i < L->n; i++) nt[i] = L->t[i];
        L->t = nt; L->cap = nc;
    }
    L->t[L->n].k = k; L->t[L->n].s = s; L->t[L->n].num = num; L->t[L->n].line = L->line;
    L->n++;
}
static int is_idstart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_idchar(char c)  { return is_idstart(c) || (c >= '0' && c <= '9'); }
static int is_digit(char c)   { return c >= '0' && c <= '9'; }

static int lex(bpy_vm *vm, const char *src, Tok **out, int *outn) {
    Lex L; L.vm = vm; L.p = src; L.line = 1; L.parens = 0;
    L.nind = 1; L.ind[0] = 0; L.t = 0; L.n = 0; L.cap = 0; L.atbol = 1;

    for (;;) {
        if (L.atbol && L.parens == 0) {
            int col = 0;
            for (;;) {
                if (*L.p == ' ')  { col++; L.p++; }
                else if (*L.p == '\t') { col += 8 - (col % 8); L.p++; }
                else break;
            }
            if (*L.p == '\n') { L.p++; L.line++; continue; }     /* blank line   */
            if (*L.p == '#')  { while (*L.p && *L.p != '\n') L.p++; continue; }
            if (*L.p == 0) break;
            if (col > L.ind[L.nind - 1]) {
                if (L.nind >= 80) { err(vm, "IndentationError: too deep"); return 1; }
                L.ind[L.nind++] = col; tok_add(&L, TK_INDENT, 0, 0);
            } else {
                while (col < L.ind[L.nind - 1]) { L.nind--; tok_add(&L, TK_DEDENT, 0, 0); }
                if (col != L.ind[L.nind - 1]) { err(vm, "IndentationError: unindent does not match any outer level"); return 1; }
            }
            L.atbol = 0;
        }

        char c = *L.p;
        if (c == 0) break;
        if (c == '\n') {
            L.p++; L.line++;
            if (L.parens == 0) { tok_add(&L, TK_NL, 0, 0); L.atbol = 1; }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { L.p++; continue; }
        if (c == '#') { while (*L.p && *L.p != '\n') L.p++; continue; }
        if (c == '\\' && L.p[1] == '\n') { L.p += 2; L.line++; continue; }

        if (is_idstart(c)) {
            const char *st = L.p;
            while (is_idchar(*L.p)) L.p++;
            tok_add(&L, TK_NAME, adup(vm, st, (uint32_t)(L.p - st)), 0);
            continue;
        }
        if (is_digit(c)) {
            int64_t val = 0;
            if (c == '0' && (L.p[1] == 'x' || L.p[1] == 'X')) {
                L.p += 2;
                while (1) {
                    char d = *L.p; int h;
                    if (d >= '0' && d <= '9') h = d - '0';
                    else if (d >= 'a' && d <= 'f') h = d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') h = d - 'A' + 10;
                    else if (d == '_') { L.p++; continue; }
                    else break;
                    val = val * 16 + h; L.p++;
                }
            } else if (c == '0' && (L.p[1] == 'b' || L.p[1] == 'B')) {
                L.p += 2;
                while (*L.p == '0' || *L.p == '1' || *L.p == '_') { if (*L.p != '_') val = val * 2 + (*L.p - '0'); L.p++; }
            } else {
                while (is_digit(*L.p) || *L.p == '_') { if (*L.p != '_') val = val * 10 + (*L.p - '0'); L.p++; }
                if (*L.p == '.' || *L.p == 'e' || *L.p == 'E') {
                    err(vm, "SyntaxError: floating point is not supported (BoltPython is integer-only)");
                    return 1;
                }
            }
            tok_add(&L, TK_NUM, 0, val);
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; L.p++;
            SB sb; sb_init(&sb, vm);
            while (*L.p && *L.p != q) {
                char d = *L.p++;
                if (d == '\\') {
                    char e = *L.p++;
                    switch (e) {
                    case 'n': sb_putc(&sb, '\n'); break;
                    case 't': sb_putc(&sb, '\t'); break;
                    case 'r': sb_putc(&sb, '\r'); break;
                    case '0': sb_putc(&sb, '\0'); break;
                    case '\\': sb_putc(&sb, '\\'); break;
                    case '\'': sb_putc(&sb, '\''); break;
                    case '"': sb_putc(&sb, '"'); break;
                    case 0: L.p--; break;
                    default: sb_putc(&sb, e); break;
                    }
                } else sb_putc(&sb, d);
            }
            if (*L.p != q) { err(vm, "SyntaxError: unterminated string literal"); return 1; }
            L.p++;
            Str *st = (Str *)aalloc(vm, sizeof(Str));
            st->len = sb.len; st->b = adup(vm, sb.b, sb.len);
            /* store as TK_STR: pointer kept in .s but length may include NULs;
             * use a Str* re-packed into s via separate token field. Simplify:
             * stash Str* in .num as pointer. */
            tok_add(&L, TK_STR, (const char *)st, 0);
            continue;
        }
        /* operators / punctuation (longest match first) */
        {
            static const char *o3[] = { "//=", "**=", "<<=", ">>=", 0 };
            static const char *o2[] = { "**", "//", "<<", ">>", "<=", ">=", "==", "!=",
                                        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", 0 };
            const char *match = 0;
            for (int i = 0; o3[i]; i++)
                if (L.p[0] == o3[i][0] && L.p[1] == o3[i][1] && L.p[2] == o3[i][2]) { match = o3[i]; break; }
            if (!match)
                for (int i = 0; o2[i]; i++)
                    if (L.p[0] == o2[i][0] && L.p[1] == o2[i][1]) { match = o2[i]; break; }
            if (match) { tok_add(&L, TK_OP, match, 0); L.p += strlen(match); continue; }

            if (c == '(' || c == '[' || c == '{') L.parens++;
            if ((c == ')' || c == ']' || c == '}') && L.parens > 0) L.parens--;
            char one[2] = { c, 0 };
            tok_add(&L, TK_OP, adup(vm, one, 1), 0);
            L.p++;
            continue;
        }
    }

    if (L.n > 0 && L.t[L.n - 1].k != TK_NL && L.parens == 0) tok_add(&L, TK_NL, 0, 0);
    while (L.nind > 1) { L.nind--; tok_add(&L, TK_DEDENT, 0, 0); }
    tok_add(&L, TK_EOF, 0, 0);
    *out = L.t; *outn = L.n;
    return vm->error;
}

/* ---------------------------------------------------------------------------
 *  Parser
 * ------------------------------------------------------------------------- */
typedef struct { bpy_vm *vm; Tok *t; int i; int n; } P;

static Tok *cur(P *p)  { return &p->t[p->i]; }
static Tok *nxt(P *p)  { return p->i + 1 < p->n ? &p->t[p->i + 1] : &p->t[p->n - 1]; }
static void adv(P *p)  { if (p->i < p->n - 1) p->i++; }
static int  is_op(P *p, const char *s)  { return cur(p)->k == TK_OP && strcmp(cur(p)->s, s) == 0; }
static int  is_kw(P *p, const char *s)  { return cur(p)->k == TK_NAME && strcmp(cur(p)->s, s) == 0; }
static int  accept_op(P *p, const char *s) { if (is_op(p, s)) { adv(p); return 1; } return 0; }

static void perr(P *p, const char *msg) {
    if (p->vm->error) return;
    char buf[160]; int i = 0;
    const char *pre = "SyntaxError: ";
    for (const char *q = pre; *q && i < 150; q++) buf[i++] = *q;
    for (const char *q = msg; *q && i < 140; q++) buf[i++] = *q;
    buf[i++] = ' '; buf[i++] = '('; buf[i++] = 'l'; buf[i++] = 'i'; buf[i++] = 'n'; buf[i++] = 'e'; buf[i++] = ' ';
    char nb[24]; i64str(cur(p)->line, nb);
    for (const char *q = nb; *q && i < 158; q++) buf[i++] = *q;
    buf[i++] = ')'; buf[i] = 0;
    err(p->vm, buf);
}
static void expect_op(P *p, const char *s) {
    if (!accept_op(p, s)) { perr(p, "expected something"); }
}

static Node *nnew(bpy_vm *vm, int k) { Node *n = (Node *)aalloc(vm, sizeof(Node)); n->k = (uint8_t)k; return n; }

static Node *parse_expr(P *p);
static Node *parse_stmt(P *p);
static Node *parse_suite(P *p);

/* node-list helper (append to kids) */
static void kids_add(bpy_vm *vm, Node *parent, Node *child) {
    Node **na = (Node **)aalloc(vm, (uint32_t)(parent->nk + 1) * sizeof(Node *));
    for (int i = 0; i < parent->nk; i++) na[i] = parent->kids[i];
    na[parent->nk++] = child; parent->kids = na;
}

static Node *parse_atom(P *p) {
    bpy_vm *vm = p->vm;
    Tok *t = cur(p);
    if (t->k == TK_NUM)  { adv(p); Node *n = nnew(vm, ND_NUM); n->num = t->num; return n; }
    if (t->k == TK_STR)  { adv(p); Node *n = nnew(vm, ND_STR); n->s = (const char *)t->s; return n; } /* s holds Str* */
    if (t->k == TK_NAME) {
        adv(p);
        if (strcmp(t->s, "True") == 0)  return nnew(vm, ND_TRUE);
        if (strcmp(t->s, "False") == 0) return nnew(vm, ND_FALSE);
        if (strcmp(t->s, "None") == 0)  return nnew(vm, ND_NONE);
        Node *n = nnew(vm, ND_NAME); n->s = t->s; return n;
    }
    if (is_op(p, "(")) { adv(p); Node *e = parse_expr(p); expect_op(p, ")"); return e; }
    if (is_op(p, "[")) {
        adv(p); Node *n = nnew(vm, ND_LIST);
        if (!is_op(p, "]")) {
            kids_add(vm, n, parse_expr(p));
            while (accept_op(p, ",")) { if (is_op(p, "]")) break; kids_add(vm, n, parse_expr(p)); }
        }
        expect_op(p, "]");
        return n;
    }
    perr(p, "unexpected token in expression");
    adv(p);
    return nnew(vm, ND_NONE);
}

static Node *parse_postfix(P *p) {
    bpy_vm *vm = p->vm;
    Node *a = parse_atom(p);
    for (;;) {
        if (is_op(p, "(")) {
            adv(p);
            Node *call = nnew(vm, ND_CALL); call->a = a;
            while (!is_op(p, ")") && cur(p)->k != TK_EOF) {
                if (cur(p)->k == TK_NAME && nxt(p)->k == TK_OP && strcmp(nxt(p)->s, "=") == 0) {
                    const char *kwname = cur(p)->s; adv(p); adv(p);
                    Node *val = parse_expr(p);
                    char **nk = (char **)aalloc(vm, (uint32_t)(call->nkw + 1) * sizeof(char *));
                    Node **nv = (Node **)aalloc(vm, (uint32_t)(call->nkw + 1) * sizeof(Node *));
                    for (int i = 0; i < call->nkw; i++) { nk[i] = call->kw[i]; nv[i] = call->kwv[i]; }
                    nk[call->nkw] = (char *)kwname; nv[call->nkw] = val; call->nkw++;
                    call->kw = nk; call->kwv = nv;
                } else {
                    kids_add(vm, call, parse_expr(p));
                }
                if (!accept_op(p, ",")) break;
            }
            expect_op(p, ")");
            a = call;
        } else if (is_op(p, "[")) {
            adv(p);
            Node *lo = 0, *hi = 0, *step = 0; int slice = 0;
            if (!is_op(p, ":")) lo = parse_expr(p);
            if (accept_op(p, ":")) {
                slice = 1;
                if (!is_op(p, ":") && !is_op(p, "]")) hi = parse_expr(p);
                if (accept_op(p, ":")) { if (!is_op(p, "]")) step = parse_expr(p); }
            }
            expect_op(p, "]");
            if (slice) { Node *n = nnew(vm, ND_SLICE); n->a = a; n->b = lo; n->c = hi; n->d = step; a = n; }
            else       { Node *n = nnew(vm, ND_INDEX); n->a = a; n->b = lo; a = n; }
        } else if (is_op(p, ".")) {
            adv(p);
            if (cur(p)->k != TK_NAME) { perr(p, "expected attribute name"); break; }
            Node *n = nnew(vm, ND_ATTR); n->a = a; n->s = cur(p)->s; adv(p); a = n;
        } else break;
    }
    return a;
}

static Node *parse_factor(P *p) {
    bpy_vm *vm = p->vm;
    if (is_op(p, "+") || is_op(p, "-") || is_op(p, "~")) {
        const char *op = cur(p)->s; adv(p);
        Node *n = nnew(vm, ND_UNARY); n->s = op; n->a = parse_factor(p); return n;
    }
    Node *base = parse_postfix(p);
    if (is_op(p, "**")) { adv(p); Node *n = nnew(vm, ND_BIN); n->s = "**"; n->a = base; n->b = parse_factor(p); return n; }
    return base;
}
static Node *bin_chain(P *p, Node *(*sub)(P *), const char **ops) {
    bpy_vm *vm = p->vm;
    Node *l = sub(p);
    for (;;) {
        const char *m = 0;
        for (int i = 0; ops[i]; i++) if (is_op(p, ops[i])) { m = ops[i]; break; }
        if (!m) break;
        adv(p);
        Node *n = nnew(vm, ND_BIN); n->s = m; n->a = l; n->b = sub(p); l = n;
    }
    return l;
}
static Node *parse_term(P *p)  { static const char *o[] = { "*", "/", "//", "%", 0 }; return bin_chain(p, parse_factor, o); }
static Node *parse_arith(P *p) { static const char *o[] = { "+", "-", 0 }; return bin_chain(p, parse_term, o); }
static Node *parse_shift(P *p) { static const char *o[] = { "<<", ">>", 0 }; return bin_chain(p, parse_arith, o); }
static Node *parse_band(P *p)  { static const char *o[] = { "&", 0 }; return bin_chain(p, parse_shift, o); }
static Node *parse_bxor(P *p)  { static const char *o[] = { "^", 0 }; return bin_chain(p, parse_band, o); }
static Node *parse_bor(P *p)   { static const char *o[] = { "|", 0 }; return bin_chain(p, parse_bxor, o); }

static Node *parse_cmp(P *p) {
    bpy_vm *vm = p->vm;
    Node *left = parse_bor(p);
    Node *result = 0, *prev = left;
    for (;;) {
        const char *op = 0;
        if (is_op(p, "<")) op = "<"; else if (is_op(p, ">")) op = ">";
        else if (is_op(p, "<=")) op = "<="; else if (is_op(p, ">=")) op = ">=";
        else if (is_op(p, "==")) op = "=="; else if (is_op(p, "!=")) op = "!=";
        else if (is_kw(p, "in")) op = "in";
        else if (is_kw(p, "not") && nxt(p)->k == TK_NAME && strcmp(nxt(p)->s, "in") == 0) op = "not in";
        else if (is_kw(p, "is")) op = "is";
        else break;
        if (op[0] == 'n' && op[1] == 'o') { adv(p); adv(p); }   /* "not in" */
        else if (op[0] == 'i' && op[1] == 's') { adv(p); if (is_kw(p, "not")) { adv(p); op = "is not"; } }
        else adv(p);
        Node *rhs = parse_bor(p);
        Node *cmp = nnew(vm, ND_BIN); cmp->s = op; cmp->a = prev; cmp->b = rhs;
        if (!result) result = cmp;
        else { Node *an = nnew(vm, ND_BOOL); an->s = "and"; an->a = result; an->b = cmp; result = an; }
        prev = rhs;
    }
    return result ? result : left;
}
static Node *parse_not(P *p) {
    if (is_kw(p, "not")) { adv(p); Node *n = nnew(p->vm, ND_NOT); n->a = parse_not(p); return n; }
    return parse_cmp(p);
}
static Node *parse_and(P *p) {
    Node *l = parse_not(p);
    while (is_kw(p, "and")) { adv(p); Node *n = nnew(p->vm, ND_BOOL); n->s = "and"; n->a = l; n->b = parse_not(p); l = n; }
    return l;
}
static Node *parse_or(P *p) {
    Node *l = parse_and(p);
    while (is_kw(p, "or")) { adv(p); Node *n = nnew(p->vm, ND_BOOL); n->s = "or"; n->a = l; n->b = parse_and(p); l = n; }
    return l;
}
static Node *parse_expr(P *p) {       /* ternary: A if C else B */
    Node *e = parse_or(p);
    if (is_kw(p, "if")) {
        adv(p);
        Node *cond = parse_or(p);
        if (!is_kw(p, "else")) { perr(p, "expected 'else' in conditional expression"); return e; }
        adv(p);
        Node *els = parse_expr(p);
        Node *n = nnew(p->vm, ND_TERNARY); n->a = cond; n->b = e; n->c = els; return n;
    }
    return e;
}

static int is_aug(P *p, char *base) {
    static const char *augs[] = { "+=", "-=", "*=", "/=", "//=", "%=", "**=", "&=", "|=", "^=", "<<=", ">>=", 0 };
    if (cur(p)->k != TK_OP) return 0;
    for (int i = 0; augs[i]; i++)
        if (strcmp(cur(p)->s, augs[i]) == 0) {
            int j = 0; while (augs[i][j] != '=') { base[j] = augs[i][j]; j++; }
            base[j] = 0; return 1;
        }
    return 0;
}

static void consume_nl(P *p) {
    if (cur(p)->k == TK_NL) adv(p);
    else if (cur(p)->k != TK_EOF && cur(p)->k != TK_DEDENT) perr(p, "expected end of line");
}

static Node *parse_small(P *p) {
    bpy_vm *vm = p->vm;
    if (is_kw(p, "pass"))     { adv(p); return nnew(vm, ND_PASS); }
    if (is_kw(p, "break"))    { adv(p); return nnew(vm, ND_BREAK); }
    if (is_kw(p, "continue")) { adv(p); return nnew(vm, ND_CONTINUE); }
    if (is_kw(p, "return")) {
        adv(p); Node *n = nnew(vm, ND_RETURN);
        if (cur(p)->k != TK_NL && cur(p)->k != TK_EOF && !is_op(p, ";")) n->a = parse_expr(p);
        return n;
    }
    if (is_kw(p, "global")) {
        adv(p); Node *n = nnew(vm, ND_GLOBAL);
        for (;;) {
            if (cur(p)->k != TK_NAME) break;
            char **na = (char **)aalloc(vm, (uint32_t)(n->nn + 1) * sizeof(char *));
            for (int i = 0; i < n->nn; i++) na[i] = n->names[i];
            na[n->nn++] = (char *)cur(p)->s; n->names = na; adv(p);
            if (!accept_op(p, ",")) break;
        }
        return n;
    }
    if (is_kw(p, "import")) { adv(p); while (cur(p)->k != TK_NL && cur(p)->k != TK_EOF) adv(p); return nnew(vm, ND_PASS); }
    if (is_kw(p, "from"))   { adv(p); while (cur(p)->k != TK_NL && cur(p)->k != TK_EOF) adv(p); return nnew(vm, ND_PASS); }

    Node *e = parse_expr(p);
    char base[8];
    if (is_aug(p, base)) {
        adv(p);
        Node *n = nnew(vm, ND_AUG); n->s = adups(vm, base); n->a = e; n->b = parse_expr(p); return n;
    }
    if (is_op(p, "=")) {
        Node *n = nnew(vm, ND_ASSIGN);
        kids_add(vm, n, e);
        Node *val = 0;
        while (accept_op(p, "=")) { val = parse_expr(p); if (is_op(p, "=")) kids_add(vm, n, val); }
        n->a = val;
        return n;
    }
    Node *n = nnew(vm, ND_EXPRSTMT); n->a = e; return n;
}

static Node *parse_simple_line(P *p) {
    bpy_vm *vm = p->vm;
    Node *first = parse_small(p);
    if (!is_op(p, ";")) { consume_nl(p); return first; }
    Node *blk = nnew(vm, ND_BLOCK); kids_add(vm, blk, first);
    while (accept_op(p, ";")) {
        if (cur(p)->k == TK_NL || cur(p)->k == TK_EOF) break;
        kids_add(vm, blk, parse_small(p));
    }
    consume_nl(p);
    return blk;
}

static Node *parse_suite(P *p) {        /* called after ':' is consumed */
    bpy_vm *vm = p->vm;
    if (cur(p)->k == TK_NL) {
        adv(p);
        if (cur(p)->k != TK_INDENT) { perr(p, "expected an indented block"); return nnew(vm, ND_BLOCK); }
        adv(p);
        Node *blk = nnew(vm, ND_BLOCK);
        while (cur(p)->k != TK_DEDENT && cur(p)->k != TK_EOF) {
            if (cur(p)->k == TK_NL) { adv(p); continue; }
            kids_add(vm, blk, parse_stmt(p));
            if (vm->error) break;
        }
        if (cur(p)->k == TK_DEDENT) adv(p);
        return blk;
    }
    return parse_simple_line(p);        /* one-liner suite */
}

static Node *parse_if(P *p) {
    bpy_vm *vm = p->vm;
    adv(p);                              /* 'if' */
    Node *root = nnew(vm, ND_IF);
    root->a = parse_expr(p); expect_op(p, ":"); root->b = parse_suite(p);
    Node *tail = root;
    while (is_kw(p, "elif")) {
        adv(p);
        Node *e = nnew(vm, ND_IF);
        e->a = parse_expr(p); expect_op(p, ":"); e->b = parse_suite(p);
        tail->c = e; tail = e;
    }
    if (is_kw(p, "else")) { adv(p); expect_op(p, ":"); tail->c = parse_suite(p); }
    return root;
}
static Node *parse_while(P *p) {
    bpy_vm *vm = p->vm;
    adv(p);
    Node *n = nnew(vm, ND_WHILE);
    n->a = parse_expr(p); expect_op(p, ":"); n->b = parse_suite(p);
    return n;
}
static Node *parse_for(P *p) {
    bpy_vm *vm = p->vm;
    adv(p);
    Node *n = nnew(vm, ND_FOR);
    for (;;) {
        if (cur(p)->k != TK_NAME) { perr(p, "expected loop variable"); break; }
        char **na = (char **)aalloc(vm, (uint32_t)(n->nn + 1) * sizeof(char *));
        for (int i = 0; i < n->nn; i++) na[i] = n->names[i];
        na[n->nn++] = (char *)cur(p)->s; n->names = na; adv(p);
        if (!accept_op(p, ",")) break;
    }
    if (!is_kw(p, "in")) { perr(p, "expected 'in'"); } else adv(p);
    n->a = parse_expr(p); expect_op(p, ":"); n->b = parse_suite(p);
    return n;
}
static Node *parse_def(P *p) {
    bpy_vm *vm = p->vm;
    adv(p);
    Node *n = nnew(vm, ND_DEF);
    if (cur(p)->k != TK_NAME) { perr(p, "expected function name"); return n; }
    n->s = cur(p)->s; adv(p);
    expect_op(p, "(");
    while (cur(p)->k == TK_NAME) {
        char **na = (char **)aalloc(vm, (uint32_t)(n->nn + 1) * sizeof(char *));
        for (int i = 0; i < n->nn; i++) na[i] = n->names[i];
        na[n->nn++] = (char *)cur(p)->s; n->names = na; adv(p);
        if (!accept_op(p, ",")) break;
    }
    expect_op(p, ")"); expect_op(p, ":");
    n->b = parse_suite(p);
    return n;
}

static Node *parse_stmt(P *p) {
    if (is_kw(p, "if"))    return parse_if(p);
    if (is_kw(p, "while")) return parse_while(p);
    if (is_kw(p, "for"))   return parse_for(p);
    if (is_kw(p, "def"))   return parse_def(p);
    return parse_simple_line(p);
}

static Node *parse_program(bpy_vm *vm, Tok *t, int n) {
    P p; p.vm = vm; p.t = t; p.i = 0; p.n = n;
    Node *blk = nnew(vm, ND_BLOCK);
    while (cur(&p)->k != TK_EOF) {
        if (cur(&p)->k == TK_NL) { adv(&p); continue; }
        kids_add(vm, blk, parse_stmt(&p));
        if (vm->error) break;
    }
    return blk;
}

/* ---------------------------------------------------------------------------
 *  Scopes
 * ------------------------------------------------------------------------- */
static int scope_find(Scope *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (strcmp(s->names[i], name) == 0) return i;
    return -1;
}
static void scope_set(bpy_vm *vm, Scope *s, const char *name, Value v) {
    int i = scope_find(s, name);
    if (i >= 0) { s->vals[i] = v; return; }
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        char **nn = (char **)aalloc(vm, (uint32_t)nc * sizeof(char *));
        Value *nv = (Value *)aalloc(vm, (uint32_t)nc * sizeof(Value));
        for (int k = 0; k < s->n; k++) { nn[k] = s->names[k]; nv[k] = s->vals[k]; }
        s->names = nn; s->vals = nv; s->cap = nc;
    }
    s->names[s->n] = (char *)name; s->vals[s->n] = v; s->n++;
}
static int scope_is_global(Scope *s, const char *name) {
    for (int i = 0; i < s->ngdecl; i++) if (strcmp(s->gdecl[i], name) == 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 *  evaluator helpers
 * ------------------------------------------------------------------------- */
static int64_t ipow(int64_t b, int64_t e) {
    int64_t r = 1; while (e > 0) { if (e & 1) r *= b; b *= b; e >>= 1; } return r;
}
static int64_t ifloordiv(int64_t a, int64_t b) {       /* python floor division */
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}
static int64_t imod(int64_t a, int64_t b) {            /* python modulo */
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

static Value str_concat(bpy_vm *vm, Str *a, Str *b) {
    Str *r = (Str *)aalloc(vm, sizeof(Str));
    r->len = a->len + b->len; r->b = (char *)aalloc(vm, r->len + 1);
    for (uint32_t i = 0; i < a->len; i++) r->b[i] = a->b[i];
    for (uint32_t i = 0; i < b->len; i++) r->b[a->len + i] = b->b[i];
    r->b[r->len] = 0;
    Value v; v.t = VT_STR; v.u.s = r; return v;
}
static Value str_repeat(bpy_vm *vm, Str *a, int64_t n) {
    if (n < 0) n = 0;
    Str *r = (Str *)aalloc(vm, sizeof(Str));
    r->len = a->len * (uint32_t)n; r->b = (char *)aalloc(vm, r->len + 1);
    uint32_t k = 0; for (int64_t t = 0; t < n; t++) for (uint32_t i = 0; i < a->len; i++) r->b[k++] = a->b[i];
    r->b[r->len] = 0;
    Value v; v.t = VT_STR; v.u.s = r; return v;
}
static Value list_concat(bpy_vm *vm, List *a, List *b) {
    List *r = list_new(vm, a->len + b->len + 1);
    for (uint32_t i = 0; i < a->len; i++) list_push(vm, r, a->items[i]);
    for (uint32_t i = 0; i < b->len; i++) list_push(vm, r, b->items[i]);
    return v_list(r);
}
static Value list_repeat(bpy_vm *vm, List *a, int64_t n) {
    if (n < 0) n = 0;
    List *r = list_new(vm, a->len * (uint32_t)n + 1);
    for (int64_t t = 0; t < n; t++) for (uint32_t i = 0; i < a->len; i++) list_push(vm, r, a->items[i]);
    return v_list(r);
}

static Value binop(bpy_vm *vm, const char *op, Value a, Value b) {
    int an = (a.t == VT_INT || a.t == VT_BOOL), bn = (b.t == VT_INT || b.t == VT_BOOL);

    if (strcmp(op, "==") == 0) return v_bool(val_eq(a, b));
    if (strcmp(op, "!=") == 0) return v_bool(!val_eq(a, b));
    if (strcmp(op, "is") == 0) return v_bool(val_eq(a, b));
    if (strcmp(op, "is not") == 0) return v_bool(!val_eq(a, b));

    if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
        int ok; int c = val_cmp(a, b, &ok);
        if (!ok) { err2(vm, "TypeError: unorderable types: ", type_name(a)); return v_none(); }
        if (op[0] == '<') return v_bool(op[1] == '=' ? c <= 0 : c < 0);
        return v_bool(op[1] == '=' ? c >= 0 : c > 0);
    }

    if (an && bn) {
        int64_t x = a.u.i, y = b.u.i;
        if (strcmp(op, "+") == 0) return v_int(x + y);
        if (strcmp(op, "-") == 0) return v_int(x - y);
        if (strcmp(op, "*") == 0) return v_int(x * y);
        if (strcmp(op, "/") == 0 || strcmp(op, "//") == 0) {
            if (y == 0) { err(vm, "ZeroDivisionError: division by zero"); return v_none(); }
            return v_int(ifloordiv(x, y));
        }
        if (strcmp(op, "%") == 0) {
            if (y == 0) { err(vm, "ZeroDivisionError: modulo by zero"); return v_none(); }
            return v_int(imod(x, y));
        }
        if (strcmp(op, "**") == 0) {
            if (y < 0) { err(vm, "ValueError: negative exponent (integer-only)"); return v_none(); }
            return v_int(ipow(x, y));
        }
        if (strcmp(op, "&") == 0)  return v_int(x & y);
        if (strcmp(op, "|") == 0)  return v_int(x | y);
        if (strcmp(op, "^") == 0)  return v_int(x ^ y);
        if (strcmp(op, "<<") == 0) return v_int(x << y);
        if (strcmp(op, ">>") == 0) return v_int(x >> y);
    }

    if (strcmp(op, "+") == 0) {
        if (a.t == VT_STR && b.t == VT_STR) return str_concat(vm, a.u.s, b.u.s);
        if (a.t == VT_LIST && b.t == VT_LIST) return list_concat(vm, a.u.l, b.u.l);
    }
    if (strcmp(op, "*") == 0) {
        if (a.t == VT_STR && bn) return str_repeat(vm, a.u.s, b.u.i);
        if (an && b.t == VT_STR) return str_repeat(vm, b.u.s, a.u.i);
        if (a.t == VT_LIST && bn) return list_repeat(vm, a.u.l, b.u.i);
        if (an && b.t == VT_LIST) return list_repeat(vm, b.u.l, a.u.i);
    }
    if (strcmp(op, "%") == 0 && a.t == VT_STR) {
        /* very small printf-less %s/%d formatting: "x=%d" % v  or % (a,b) */
        SB s; sb_init(&s, vm);
        List tmp; Value one[1];
        List *args; if (b.t == VT_LIST) args = b.u.l; else { one[0] = b; tmp.items = one; tmp.len = 1; args = &tmp; }
        uint32_t ai = 0;
        for (uint32_t i = 0; i < a.u.s->len; i++) {
            char c = a.u.s->b[i];
            if (c == '%' && i + 1 < a.u.s->len) {
                char d = a.u.s->b[++i];
                if (d == '%') { sb_putc(&s, '%'); continue; }
                Value arg = ai < args->len ? args->items[ai++] : v_none();
                if (d == 'd' || d == 'i') sb_puti(&s, arg.t == VT_STR ? 0 : arg.u.i);
                else { SB t2; sb_init(&t2, vm); vfmt(vm, &t2, arg, 0); sb_putn(&s, t2.b, t2.len); }
            } else sb_putc(&s, c);
        }
        return v_strn(vm, s.b, s.len);
    }

    {
        char m[64]; int k = 0;
        const char *pre = "TypeError: unsupported operand type(s) for ";
        for (const char *q = pre; *q && k < 50; q++) m[k++] = *q;
        for (const char *q = op; *q && k < 60; q++) m[k++] = *q;
        m[k] = 0; err(vm, m);
    }
    return v_none();
}

/* normalise + bounds-check a single index for str/list (python semantics) */
static int norm_index(int64_t idx, uint32_t len, uint32_t *out) {
    if (idx < 0) idx += (int64_t)len;
    if (idx < 0 || idx >= (int64_t)len) return 0;
    *out = (uint32_t)idx; return 1;
}
/* clamp a slice bound */
static int64_t clamp_slice(int64_t v, int64_t len, int def) {
    if (def && v == INT64_MIN) return 0;
    if (v < 0) v += len;
    if (v < 0) v = 0;
    if (v > len) v = len;
    return v;
}

static Value do_index(bpy_vm *vm, Value base, Value idx) {
    if (idx.t != VT_INT && idx.t != VT_BOOL) { err(vm, "TypeError: indices must be integers"); return v_none(); }
    uint32_t k;
    if (base.t == VT_STR) {
        if (!norm_index(idx.u.i, base.u.s->len, &k)) { err(vm, "IndexError: string index out of range"); return v_none(); }
        return v_strn(vm, base.u.s->b + k, 1);
    }
    if (base.t == VT_LIST) {
        if (!norm_index(idx.u.i, base.u.l->len, &k)) { err(vm, "IndexError: list index out of range"); return v_none(); }
        return base.u.l->items[k];
    }
    err2(vm, "TypeError: not subscriptable: ", type_name(base));
    return v_none();
}

static Value do_slice(bpy_vm *vm, Value base, Value lo, Value hi, Value step, int hl, int hh, int hs) {
    int64_t len;
    if (base.t == VT_STR) len = base.u.s->len;
    else if (base.t == VT_LIST) len = base.u.l->len;
    else { err(vm, "TypeError: object is not sliceable"); return v_none(); }

    int64_t st = hs ? step.u.i : 1;
    if (st == 0) { err(vm, "ValueError: slice step cannot be zero"); return v_none(); }
    int64_t start, stop;
    if (st > 0) {
        start = hl ? clamp_slice(lo.u.i, len, 0) : 0;
        stop  = hh ? clamp_slice(hi.u.i, len, 0) : len;
    } else {
        start = hl ? lo.u.i : len - 1;
        stop  = hh ? hi.u.i : -1 - 0;
        if (hl && start < 0) start += len; if (start > len - 1) start = len - 1; if (start < -1) start = -1;
        if (hh) { if (stop < 0) stop += len; if (stop < -1) stop = -1; if (stop > len) stop = len; }
        else stop = -1;
    }
    if (base.t == VT_STR) {
        SB s; sb_init(&s, vm);
        if (st > 0) for (int64_t i = start; i < stop; i += st) sb_putc(&s, base.u.s->b[i]);
        else        for (int64_t i = start; i > stop; i += st) sb_putc(&s, base.u.s->b[i]);
        return v_strn(vm, s.b, s.len);
    } else {
        List *r = list_new(vm, 4);
        if (st > 0) for (int64_t i = start; i < stop; i += st) list_push(vm, r, base.u.l->items[i]);
        else        for (int64_t i = start; i > stop; i += st) list_push(vm, r, base.u.l->items[i]);
        return v_list(r);
    }
}

/* membership test for "in" */
static int do_contains(bpy_vm *vm, Value item, Value cont) {
    if (cont.t == VT_LIST) {
        for (uint32_t i = 0; i < cont.u.l->len; i++) if (val_eq(item, cont.u.l->items[i])) return 1;
        return 0;
    }
    if (cont.t == VT_STR && item.t == VT_STR) {
        Str *h = cont.u.s, *n = item.u.s;
        if (n->len == 0) return 1;
        if (n->len > h->len) return 0;
        for (uint32_t i = 0; i + n->len <= h->len; i++) {
            uint32_t j = 0; while (j < n->len && h->b[i + j] == n->b[j]) j++;
            if (j == n->len) return 1;
        }
        return 0;
    }
    err(vm, "TypeError: argument is not iterable for 'in'");
    return 0;
}

/* ---------------------------------------------------------------------------
 *  assignment targets
 * ------------------------------------------------------------------------- */
static void assign_to(bpy_vm *vm, Node *target, Value val) {
    if (target->k == ND_NAME) {
        Scope *s = vm->cur;
        if (s != &vm->globals && scope_is_global(s, target->s)) scope_set(vm, &vm->globals, target->s, val);
        else scope_set(vm, s, target->s, val);
        return;
    }
    if (target->k == ND_INDEX) {
        Value base = eval(vm, target->a); if (vm->error) return;
        Value idx  = eval(vm, target->b); if (vm->error) return;
        if (base.t != VT_LIST) { err(vm, "TypeError: object does not support item assignment"); return; }
        if (idx.t != VT_INT && idx.t != VT_BOOL) { err(vm, "TypeError: indices must be integers"); return; }
        uint32_t k;
        if (!norm_index(idx.u.i, base.u.l->len, &k)) { err(vm, "IndexError: assignment index out of range"); return; }
        base.u.l->items[k] = val;
        return;
    }
    err(vm, "SyntaxError: cannot assign to this expression");
}

/* ---------------------------------------------------------------------------
 *  expression evaluation
 * ------------------------------------------------------------------------- */
static Value eval(bpy_vm *vm, Node *n) {
    if (vm->error) return v_none();
    switch (n->k) {
    case ND_NUM:   return v_int(n->num);
    case ND_TRUE:  return v_bool(1);
    case ND_FALSE: return v_bool(0);
    case ND_NONE:  return v_none();
    case ND_STR: { Str *st = (Str *)n->s; Value v; v.t = VT_STR; v.u.s = st; return v; }
    case ND_NAME: {
        Scope *s = vm->cur;
        int i = scope_find(s, n->s);
        if (i >= 0) return s->vals[i];
        if (s != &vm->globals) { i = scope_find(&vm->globals, n->s); if (i >= 0) return vm->globals.vals[i]; }
        int b = builtin_index(n->s);
        if (b >= 0) return v_builtin(b);
        err2(vm, "NameError: name is not defined: ", n->s);
        return v_none();
    }
    case ND_LIST: {
        List *l = list_new(vm, (uint32_t)n->nk + 1);
        for (int i = 0; i < n->nk; i++) { Value e = eval(vm, n->kids[i]); if (vm->error) return v_none(); list_push(vm, l, e); }
        return v_list(l);
    }
    case ND_UNARY: {
        Value a = eval(vm, n->a); if (vm->error) return v_none();
        if (n->s[0] == '-') { if (a.t == VT_INT || a.t == VT_BOOL) return v_int(-a.u.i); }
        if (n->s[0] == '+') { if (a.t == VT_INT || a.t == VT_BOOL) return v_int(a.u.i); }
        if (n->s[0] == '~') { if (a.t == VT_INT || a.t == VT_BOOL) return v_int(~a.u.i); }
        err(vm, "TypeError: bad operand type for unary operator"); return v_none();
    }
    case ND_NOT: { Value a = eval(vm, n->a); if (vm->error) return v_none(); return v_bool(!truthy(a)); }
    case ND_BOOL: {
        Value a = eval(vm, n->a); if (vm->error) return v_none();
        if (n->s[0] == 'a') { if (!truthy(a)) return a; return eval(vm, n->b); }   /* and */
        if (truthy(a)) return a; return eval(vm, n->b);                            /* or  */
    }
    case ND_BIN: {
        if (strcmp(n->s, "in") == 0 || strcmp(n->s, "not in") == 0) {
            Value a = eval(vm, n->a); if (vm->error) return v_none();
            Value b = eval(vm, n->b); if (vm->error) return v_none();
            int r = do_contains(vm, a, b);
            return v_bool(n->s[0] == 'n' ? !r : r);
        }
        Value a = eval(vm, n->a); if (vm->error) return v_none();
        Value b = eval(vm, n->b); if (vm->error) return v_none();
        return binop(vm, n->s, a, b);
    }
    case ND_TERNARY: {
        Value c = eval(vm, n->a); if (vm->error) return v_none();
        return truthy(c) ? eval(vm, n->b) : eval(vm, n->c);
    }
    case ND_INDEX: {
        Value base = eval(vm, n->a); if (vm->error) return v_none();
        Value idx  = eval(vm, n->b); if (vm->error) return v_none();
        return do_index(vm, base, idx);
    }
    case ND_SLICE: {
        Value base = eval(vm, n->a); if (vm->error) return v_none();
        Value lo = v_none(), hi = v_none(), step = v_none();
        int hl = 0, hh = 0, hs = 0;
        if (n->b) { lo = eval(vm, n->b); hl = 1; if (vm->error) return v_none(); }
        if (n->c) { hi = eval(vm, n->c); hh = 1; if (vm->error) return v_none(); }
        if (n->d) { step = eval(vm, n->d); hs = 1; if (vm->error) return v_none(); }
        return do_slice(vm, base, lo, hi, step, hl, hh, hs);
    }
    case ND_ATTR: {
        /* bare attribute access only meaningful as a bound method via CALL;
         * reaching here means it was used as a value -> unsupported */
        err2(vm, "AttributeError: attribute access unsupported here: ", n->s);
        return v_none();
    }
    case ND_CALL: {
        /* method call?  obj.method(args) */
        if (n->a->k == ND_ATTR) {
            Value base = eval(vm, n->a->a); if (vm->error) return v_none();
            Value args[16]; int na = n->nk; if (na > 16) na = 16;
            for (int i = 0; i < na; i++) { args[i] = eval(vm, n->kids[i]); if (vm->error) return v_none(); }
            return call_method(vm, base, n->a->s, args, na);
        }
        Value callee = eval(vm, n->a); if (vm->error) return v_none();
        Value args[16]; int na = n->nk; if (na > 16) na = 16;
        for (int i = 0; i < na; i++) { args[i] = eval(vm, n->kids[i]); if (vm->error) return v_none(); }
        Value kwv[8]; char *kwn[8]; int nkw = n->nkw; if (nkw > 8) nkw = 8;
        for (int i = 0; i < nkw; i++) { kwn[i] = n->kw[i]; kwv[i] = eval(vm, n->kwv[i]); if (vm->error) return v_none(); }
        return call_value(vm, callee, args, na, kwn, kwv, nkw);
    }
    default:
        err(vm, "SystemError: cannot evaluate node");
        return v_none();
    }
}

/* ---------------------------------------------------------------------------
 *  calling
 * ------------------------------------------------------------------------- */
static Value call_value(bpy_vm *vm, Value callee, Value *args, int nargs, char **kw, Value *kwv, int nkw) {
    if (callee.t == VT_BUILTIN) return call_builtin(vm, callee.u.bi, args, nargs, kw, kwv, nkw);
    if (callee.t == VT_FUNC) {
        Func *fn = callee.u.fn;
        if (nargs != fn->nparams) { err2(vm, "TypeError: wrong number of arguments to ", fn->name); return v_none(); }
        if (++vm->depth > MAX_DEPTH) { vm->depth--; err(vm, "RecursionError: maximum recursion depth exceeded"); return v_none(); }

        Scope local; local.names = 0; local.vals = 0; local.n = 0; local.cap = 0; local.gdecl = 0; local.ngdecl = 0;
        for (int i = 0; i < fn->nparams; i++) scope_set(vm, &local, fn->params[i], args[i]);

        Scope *save = vm->cur; int saveflow = vm->flow; Value saveret = vm->retval;
        vm->cur = &local; vm->flow = F_NONE;
        exec_block(vm, fn->body);
        Value rv = (vm->flow == F_RET) ? vm->retval : v_none();
        vm->cur = save; vm->flow = saveflow; vm->retval = saveret;
        vm->depth--;
        return rv;
    }
    err2(vm, "TypeError: object is not callable: ", type_name(callee));
    return v_none();
}

/* ---------------------------------------------------------------------------
 *  statement execution
 * ------------------------------------------------------------------------- */
static void exec_block(bpy_vm *vm, Node *blk) {
    for (int i = 0; i < blk->nk; i++) {
        if (vm->error || vm->flow != F_NONE) return;
        exec(vm, blk->kids[i]);
    }
}

static void exec(bpy_vm *vm, Node *n) {
    if (vm->error || vm->flow != F_NONE) return;
    switch (n->k) {
    case ND_PASS: case ND_GLOBAL:
        if (n->k == ND_GLOBAL) {
            for (int i = 0; i < n->nn; i++) {
                Scope *s = vm->cur;
                char **ng = (char **)aalloc(vm, (uint32_t)(s->ngdecl + 1) * sizeof(char *));
                for (int j = 0; j < s->ngdecl; j++) ng[j] = s->gdecl[j];
                ng[s->ngdecl++] = n->names[i]; s->gdecl = ng;
            }
        }
        return;
    case ND_BLOCK: exec_block(vm, n); return;
    case ND_EXPRSTMT: {
        Value v = eval(vm, n->a);
        vm->last = v; vm->last_is_expr = 1;
        return;
    }
    case ND_ASSIGN: {
        Value v = eval(vm, n->a); if (vm->error) return;
        for (int i = 0; i < n->nk; i++) { assign_to(vm, n->kids[i], v); if (vm->error) return; }
        vm->last_is_expr = 0;
        return;
    }
    case ND_AUG: {
        Value rhs = eval(vm, n->b); if (vm->error) return;
        Value cur = eval(vm, n->a); if (vm->error) return;
        Value res = binop(vm, n->s, cur, rhs); if (vm->error) return;
        assign_to(vm, n->a, res);
        vm->last_is_expr = 0;
        return;
    }
    case ND_IF: {
        Value c = eval(vm, n->a); if (vm->error) return;
        if (truthy(c)) exec(vm, n->b);
        else if (n->c) exec(vm, n->c);
        return;
    }
    case ND_WHILE: {
        for (;;) {
            Value c = eval(vm, n->a); if (vm->error) return;
            if (!truthy(c)) break;
            exec(vm, n->b);
            if (vm->error) return;
            if (vm->flow == F_BREAK) { vm->flow = F_NONE; break; }
            if (vm->flow == F_CONT) { vm->flow = F_NONE; continue; }
            if (vm->flow == F_RET) return;
        }
        return;
    }
    case ND_FOR: {
        /* fast path: for i in range(...) without materialising the list */
        Node *it = n->a;
        if (it->k == ND_CALL && it->a->k == ND_NAME && strcmp(it->a->s, "range") == 0 && n->nn == 1 && it->nk >= 1 && it->nk <= 3) {
            int64_t a0 = 0, b0 = 0, st = 1;
            Value av[3];
            for (int i = 0; i < it->nk; i++) { av[i] = eval(vm, it->kids[i]); if (vm->error) return; }
            if (it->nk == 1) { b0 = av[0].u.i; }
            else { a0 = av[0].u.i; b0 = av[1].u.i; if (it->nk == 3) st = av[2].u.i; }
            if (st == 0) { err(vm, "ValueError: range() step must not be zero"); return; }
            for (int64_t x = a0; (st > 0) ? (x < b0) : (x > b0); x += st) {
                scope_set(vm, vm->cur, n->names[0], v_int(x));
                exec(vm, n->b);
                if (vm->error) return;
                if (vm->flow == F_BREAK) { vm->flow = F_NONE; break; }
                if (vm->flow == F_CONT) { vm->flow = F_NONE; continue; }
                if (vm->flow == F_RET) return;
            }
            return;
        }
        Value seq = eval(vm, n->a); if (vm->error) return;
        uint32_t len;
        if (seq.t == VT_LIST) len = seq.u.l->len;
        else if (seq.t == VT_STR) len = seq.u.s->len;
        else { err2(vm, "TypeError: object is not iterable: ", type_name(seq)); return; }
        for (uint32_t i = 0; i < len; i++) {
            Value item = (seq.t == VT_LIST) ? seq.u.l->items[i] : v_strn(vm, seq.u.s->b + i, 1);
            if (n->nn == 1) scope_set(vm, vm->cur, n->names[0], item);
            else {
                /* unpack item (must be list/str) into n->nn names */
                if (item.t == VT_LIST && (int)item.u.l->len == n->nn)
                    for (int k = 0; k < n->nn; k++) scope_set(vm, vm->cur, n->names[k], item.u.l->items[k]);
                else { err(vm, "ValueError: cannot unpack loop item"); return; }
            }
            exec(vm, n->b);
            if (vm->error) return;
            if (vm->flow == F_BREAK) { vm->flow = F_NONE; break; }
            if (vm->flow == F_CONT) { vm->flow = F_NONE; continue; }
            if (vm->flow == F_RET) return;
        }
        return;
    }
    case ND_DEF: {
        Func *fn = (Func *)aalloc(vm, sizeof(Func));
        fn->name = n->s; fn->params = n->names; fn->nparams = n->nn; fn->body = n->b;
        Value v; v.t = VT_FUNC; v.u.fn = fn;
        scope_set(vm, vm->cur, n->s, v);
        return;
    }
    case ND_RETURN: {
        vm->retval = n->a ? eval(vm, n->a) : v_none();
        if (vm->error) return;
        vm->flow = F_RET;
        return;
    }
    case ND_BREAK:    vm->flow = F_BREAK; return;
    case ND_CONTINUE: vm->flow = F_CONT;  return;
    default:
        err(vm, "SystemError: cannot execute node");
        return;
    }
}

/* ---------------------------------------------------------------------------
 *  builtins
 * ------------------------------------------------------------------------- */
static int64_t to_int(bpy_vm *vm, Value v, int *ok) {
    *ok = 1;
    if (v.t == VT_INT || v.t == VT_BOOL) return v.u.i;
    if (v.t == VT_STR) {
        const char *s = v.u.s->b; int neg = 0; int64_t r = 0; int any = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
        while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; any = 1; }
        while (*s == ' ' || *s == '\t') s++;
        if (!any || *s) { *ok = 0; err(vm, "ValueError: invalid literal for int()"); return 0; }
        return neg ? -r : r;
    }
    *ok = 0; err(vm, "TypeError: int() argument must be a number or string"); return 0;
}
static uint32_t val_len(bpy_vm *vm, Value v, int *ok) {
    *ok = 1;
    if (v.t == VT_STR) return v.u.s->len;
    if (v.t == VT_LIST) return v.u.l->len;
    *ok = 0; err2(vm, "TypeError: object has no len(): ", type_name(v)); return 0;
}

/* builtin index constants */
enum {
    B_PRINT, B_LEN, B_RANGE, B_STR, B_INT, B_BOOL, B_ABS, B_MIN, B_MAX, B_SUM,
    B_ORD, B_CHR, B_HEX, B_TYPE, B_INPUT, B_LIST, B_REPR, B_SORTED, B_REVERSED,
    B_ENUMERATE, B_BIN, B_POW, B_GCD, B_DIVMOD, B_COUNT_
};
static const char *builtin_names[] = {
    "print", "len", "range", "str", "int", "bool", "abs", "min", "max", "sum",
    "ord", "chr", "hex", "type", "input", "list", "repr", "sorted", "reversed",
    "enumerate", "bin", "pow", "gcd", "divmod", 0
};
static int builtin_index(const char *name) {
    for (int i = 0; builtin_names[i]; i++) if (strcmp(builtin_names[i], name) == 0) return i;
    return -1;
}

static Value make_range(bpy_vm *vm, int64_t a, int64_t b, int64_t st) {
    if (st == 0) { err(vm, "ValueError: range() step must not be zero"); return v_none(); }
    List *l = list_new(vm, 8);
    int64_t cnt = 0;
    for (int64_t x = a; (st > 0) ? (x < b) : (x > b); x += st) {
        if (++cnt > RANGE_CAP) { err(vm, "OverflowError: range too large to materialise"); return v_none(); }
        list_push(vm, l, v_int(x));
    }
    return v_list(l);
}

static Value call_builtin(bpy_vm *vm, int idx, Value *a, int n, char **kw, Value *kwv, int nkw) {
    char buf[24];
    switch (idx) {
    case B_PRINT: {
        const char *sep = " "; uint32_t seplen = 1;
        const char *end = "\n"; uint32_t endlen = 1;
        for (int i = 0; i < nkw; i++) {
            if (strcmp(kw[i], "sep") == 0 && kwv[i].t == VT_STR) { sep = kwv[i].u.s->b; seplen = kwv[i].u.s->len; }
            if (strcmp(kw[i], "end") == 0 && kwv[i].t == VT_STR) { end = kwv[i].u.s->b; endlen = kwv[i].u.s->len; }
        }
        for (int i = 0; i < n; i++) {
            if (i) for (uint32_t k = 0; k < seplen; k++) kputc(sep[k]);
            print_value(vm, a[i], 0);
        }
        for (uint32_t k = 0; k < endlen; k++) kputc(end[k]);
        return v_none();
    }
    case B_LEN: { if (n != 1) { err(vm, "TypeError: len() takes one argument"); return v_none(); }
                  int ok; uint32_t l = val_len(vm, a[0], &ok); return ok ? v_int(l) : v_none(); }
    case B_RANGE: {
        if (n == 1) return make_range(vm, 0, a[0].u.i, 1);
        if (n == 2) return make_range(vm, a[0].u.i, a[1].u.i, 1);
        if (n == 3) return make_range(vm, a[0].u.i, a[1].u.i, a[2].u.i);
        err(vm, "TypeError: range() takes 1 to 3 arguments"); return v_none();
    }
    case B_STR:  { if (n == 0) return v_str(vm, ""); return to_str_value(vm, a[0], 0); }
    case B_REPR: { if (n == 0) return v_str(vm, ""); return to_str_value(vm, a[0], 1); }
    case B_INT:  { if (n == 0) return v_int(0); int ok; int64_t v = to_int(vm, a[0], &ok); return ok ? v_int(v) : v_none(); }
    case B_BOOL: { if (n == 0) return v_bool(0); return v_bool(truthy(a[0])); }
    case B_ABS:  { if (n != 1 || (a[0].t != VT_INT && a[0].t != VT_BOOL)) { err(vm, "TypeError: abs() needs a number"); return v_none(); }
                   int64_t v = a[0].u.i; return v_int(v < 0 ? -v : v); }
    case B_MIN: case B_MAX: {
        Value *items; uint32_t len;
        if (n == 1 && a[0].t == VT_LIST) { items = a[0].u.l->items; len = a[0].u.l->len; }
        else { items = a; len = (uint32_t)n; }
        if (len == 0) { err(vm, "ValueError: min()/max() arg is empty"); return v_none(); }
        Value best = items[0];
        for (uint32_t i = 1; i < len; i++) {
            int ok; int c = val_cmp(items[i], best, &ok);
            if (!ok) { err(vm, "TypeError: not comparable"); return v_none(); }
            if ((idx == B_MIN && c < 0) || (idx == B_MAX && c > 0)) best = items[i];
        }
        return best;
    }
    case B_SUM: {
        if (n < 1 || a[0].t != VT_LIST) { err(vm, "TypeError: sum() needs a list"); return v_none(); }
        int64_t s = (n == 2 && (a[1].t == VT_INT || a[1].t == VT_BOOL)) ? a[1].u.i : 0;
        for (uint32_t i = 0; i < a[0].u.l->len; i++) {
            Value e = a[0].u.l->items[i];
            if (e.t != VT_INT && e.t != VT_BOOL) { err(vm, "TypeError: sum() supports ints only"); return v_none(); }
            s += e.u.i;
        }
        return v_int(s);
    }
    case B_ORD: { if (n != 1 || a[0].t != VT_STR || a[0].u.s->len != 1) { err(vm, "TypeError: ord() needs a 1-char string"); return v_none(); }
                  return v_int((unsigned char)a[0].u.s->b[0]); }
    case B_CHR: { if (n != 1 || (a[0].t != VT_INT && a[0].t != VT_BOOL)) { err(vm, "TypeError: chr() needs an int"); return v_none(); }
                  char c = (char)(a[0].u.i & 0xFF); return v_strn(vm, &c, 1); }
    case B_HEX: { if (n != 1 || (a[0].t != VT_INT && a[0].t != VT_BOOL)) { err(vm, "TypeError: hex() needs an int"); return v_none(); }
                  int64_t v = a[0].u.i; SB s; sb_init(&s, vm); int neg = v < 0; uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
                  if (neg) sb_putc(&s, '-'); sb_puts(&s, "0x");
                  char tmp[20]; int k = 0; const char *hx = "0123456789abcdef";
                  if (!u) tmp[k++] = '0'; while (u) { tmp[k++] = hx[u & 0xF]; u >>= 4; }
                  while (k--) sb_putc(&s, tmp[k]); return v_strn(vm, s.b, s.len); }
    case B_BIN: { if (n != 1 || (a[0].t != VT_INT && a[0].t != VT_BOOL)) { err(vm, "TypeError: bin() needs an int"); return v_none(); }
                  int64_t v = a[0].u.i; SB s; sb_init(&s, vm); int neg = v < 0; uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
                  if (neg) sb_putc(&s, '-'); sb_puts(&s, "0b");
                  char tmp[68]; int k = 0; if (!u) tmp[k++] = '0'; while (u) { tmp[k++] = (char)('0' + (u & 1)); u >>= 1; }
                  while (k--) sb_putc(&s, tmp[k]); return v_strn(vm, s.b, s.len); }
    case B_POW: { if (n < 2 || (a[0].t != VT_INT && a[0].t != VT_BOOL) || (a[1].t != VT_INT && a[1].t != VT_BOOL)) { err(vm, "TypeError: pow() needs two ints"); return v_none(); }
                  int64_t base = a[0].u.i, e = a[1].u.i;
                  if (e < 0) { err(vm, "ValueError: pow() negative exponent not supported"); return v_none(); }
                  int64_t r = 1; while (e-- > 0) r *= base; return v_int(r); }
    case B_GCD: { if (n < 2 || (a[0].t != VT_INT && a[0].t != VT_BOOL) || (a[1].t != VT_INT && a[1].t != VT_BOOL)) { err(vm, "TypeError: gcd() needs two ints"); return v_none(); }
                  int64_t x = a[0].u.i, y = a[1].u.i; if (x < 0) x = -x; if (y < 0) y = -y;
                  while (y) { int64_t t = x % y; x = y; y = t; } return v_int(x); }
    case B_DIVMOD: { if (n < 2 || (a[0].t != VT_INT && a[0].t != VT_BOOL) || (a[1].t != VT_INT && a[1].t != VT_BOOL)) { err(vm, "TypeError: divmod() needs two ints"); return v_none(); }
                  int64_t x = a[0].u.i, y = a[1].u.i;
                  if (y == 0) { err(vm, "ZeroDivisionError: integer division or modulo by zero"); return v_none(); }
                  int64_t q = x / y, rmd = x % y;          /* C truncates; convert to floor */
                  if (rmd != 0 && ((rmd < 0) != (y < 0))) { q -= 1; rmd += y; }
                  List *l = list_new(vm, 2); list_push(vm, l, v_int(q)); list_push(vm, l, v_int(rmd)); return v_list(l); }
    case B_TYPE: { if (n != 1) { err(vm, "TypeError: type() takes one argument"); return v_none(); }
                   SB s; sb_init(&s, vm); sb_puts(&s, "<class '"); sb_puts(&s, type_name(a[0])); sb_puts(&s, "'>");
                   return v_strn(vm, s.b, s.len); }
    case B_INPUT: {
        if (n >= 1 && a[0].t == VT_STR) print_value(vm, a[0], 0);
        char line[256];
        int len = vm->input ? vm->input(line, sizeof(line)) : 0;
        if (len < 0) len = 0;
        return v_strn(vm, line, (uint32_t)len);
    }
    case B_LIST: {
        List *l = list_new(vm, 8);
        if (n >= 1) {
            if (a[0].t == VT_LIST) for (uint32_t i = 0; i < a[0].u.l->len; i++) list_push(vm, l, a[0].u.l->items[i]);
            else if (a[0].t == VT_STR) for (uint32_t i = 0; i < a[0].u.s->len; i++) list_push(vm, l, v_strn(vm, a[0].u.s->b + i, 1));
            else { err(vm, "TypeError: list() argument is not iterable"); return v_none(); }
        }
        return v_list(l);
    }
    case B_SORTED: {
        if (n < 1 || a[0].t != VT_LIST) { err(vm, "TypeError: sorted() needs a list"); return v_none(); }
        List *src = a[0].u.l; List *r = list_new(vm, src->len + 1);
        for (uint32_t i = 0; i < src->len; i++) list_push(vm, r, src->items[i]);
        for (uint32_t i = 1; i < r->len; i++) {     /* insertion sort (stable) */
            Value key = r->items[i]; int32_t j = (int32_t)i - 1;
            while (j >= 0) { int ok; int c = val_cmp(r->items[j], key, &ok);
                             if (!ok) { err(vm, "TypeError: unorderable elements in sorted()"); return v_none(); }
                             if (c <= 0) break; r->items[j + 1] = r->items[j]; j--; }
            r->items[j + 1] = key;
        }
        return v_list(r);
    }
    case B_REVERSED: {
        if (n < 1) { err(vm, "TypeError: reversed() needs an argument"); return v_none(); }
        List *r = list_new(vm, 8);
        if (a[0].t == VT_LIST) for (int32_t i = (int32_t)a[0].u.l->len - 1; i >= 0; i--) list_push(vm, r, a[0].u.l->items[i]);
        else if (a[0].t == VT_STR) for (int32_t i = (int32_t)a[0].u.s->len - 1; i >= 0; i--) list_push(vm, r, v_strn(vm, a[0].u.s->b + i, 1));
        else { err(vm, "TypeError: reversed() argument is not reversible"); return v_none(); }
        return v_list(r);
    }
    case B_ENUMERATE: {
        if (n < 1 || (a[0].t != VT_LIST && a[0].t != VT_STR)) { err(vm, "TypeError: enumerate() needs a sequence"); return v_none(); }
        int64_t start = (n == 2 && (a[1].t == VT_INT || a[1].t == VT_BOOL)) ? a[1].u.i : 0;
        List *r = list_new(vm, 8);
        uint32_t len = a[0].t == VT_LIST ? a[0].u.l->len : a[0].u.s->len;
        for (uint32_t i = 0; i < len; i++) {
            List *pair = list_new(vm, 2);
            list_push(vm, pair, v_int(start + i));
            list_push(vm, pair, a[0].t == VT_LIST ? a[0].u.l->items[i] : v_strn(vm, a[0].u.s->b + i, 1));
            list_push(vm, r, v_list(pair));
        }
        return v_list(r);
    }
    default:
        (void)buf;
        err(vm, "SystemError: unknown builtin");
        return v_none();
    }
}

/* ---------------------------------------------------------------------------
 *  methods on list / str
 * ------------------------------------------------------------------------- */
static Value call_method(bpy_vm *vm, Value base, const char *name, Value *a, int n) {
    if (base.t == VT_LIST) {
        List *l = base.u.l;
        if (strcmp(name, "append") == 0) { if (n != 1) { err(vm, "TypeError: append() takes one argument"); return v_none(); }
                                           list_push(vm, l, a[0]); return v_none(); }
        if (strcmp(name, "pop") == 0) {
            if (l->len == 0) { err(vm, "IndexError: pop from empty list"); return v_none(); }
            int64_t idx = (n >= 1 && (a[0].t == VT_INT || a[0].t == VT_BOOL)) ? a[0].u.i : (int64_t)l->len - 1;
            uint32_t k; if (!norm_index(idx, l->len, &k)) { err(vm, "IndexError: pop index out of range"); return v_none(); }
            Value out = l->items[k];
            for (uint32_t i = k; i + 1 < l->len; i++) l->items[i] = l->items[i + 1];
            l->len--; return out;
        }
        if (strcmp(name, "insert") == 0) {
            if (n != 2 || (a[0].t != VT_INT && a[0].t != VT_BOOL)) { err(vm, "TypeError: insert(i, x)"); return v_none(); }
            int64_t idx = a[0].u.i; if (idx < 0) idx += (int64_t)l->len; if (idx < 0) idx = 0; if (idx > (int64_t)l->len) idx = l->len;
            list_push(vm, l, v_none());
            for (uint32_t i = l->len - 1; i > (uint32_t)idx; i--) l->items[i] = l->items[i - 1];
            l->items[idx] = a[1]; return v_none();
        }
        if (strcmp(name, "index") == 0) {
            if (n != 1) { err(vm, "TypeError: index(x)"); return v_none(); }
            for (uint32_t i = 0; i < l->len; i++) if (val_eq(l->items[i], a[0])) return v_int(i);
            err(vm, "ValueError: item is not in list"); return v_none();
        }
        if (strcmp(name, "count") == 0) {
            if (n != 1) { err(vm, "TypeError: count(x)"); return v_none(); }
            int64_t c = 0; for (uint32_t i = 0; i < l->len; i++) if (val_eq(l->items[i], a[0])) c++;
            return v_int(c);
        }
        if (strcmp(name, "reverse") == 0) {
            for (uint32_t i = 0, j = l->len ? l->len - 1 : 0; i < j; i++, j--) { Value t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }
            return v_none();
        }
        if (strcmp(name, "clear") == 0) { l->len = 0; return v_none(); }
        err2(vm, "AttributeError: 'list' object has no method ", name); return v_none();
    }
    if (base.t == VT_STR) {
        Str *s = base.u.s;
        if (strcmp(name, "upper") == 0) { SB b; sb_init(&b, vm); for (uint32_t i = 0; i < s->len; i++) { char c = s->b[i]; if (c >= 'a' && c <= 'z') c -= 32; sb_putc(&b, c); } return v_strn(vm, b.b, b.len); }
        if (strcmp(name, "lower") == 0) { SB b; sb_init(&b, vm); for (uint32_t i = 0; i < s->len; i++) { char c = s->b[i]; if (c >= 'A' && c <= 'Z') c += 32; sb_putc(&b, c); } return v_strn(vm, b.b, b.len); }
        if (strcmp(name, "strip") == 0) {
            uint32_t i = 0, j = s->len; while (i < j && (s->b[i] == ' ' || s->b[i] == '\t' || s->b[i] == '\n' || s->b[i] == '\r')) i++;
            while (j > i && (s->b[j-1] == ' ' || s->b[j-1] == '\t' || s->b[j-1] == '\n' || s->b[j-1] == '\r')) j--;
            return v_strn(vm, s->b + i, j - i);
        }
        if (strcmp(name, "split") == 0) {
            List *r = list_new(vm, 8);
            if (n == 0) {       /* split on whitespace runs */
                uint32_t i = 0;
                while (i < s->len) {
                    while (i < s->len && (s->b[i] == ' ' || s->b[i] == '\t' || s->b[i] == '\n' || s->b[i] == '\r')) i++;
                    uint32_t st = i;
                    while (i < s->len && !(s->b[i] == ' ' || s->b[i] == '\t' || s->b[i] == '\n' || s->b[i] == '\r')) i++;
                    if (i > st) list_push(vm, r, v_strn(vm, s->b + st, i - st));
                }
            } else if (a[0].t == VT_STR && a[0].u.s->len > 0) {
                Str *d = a[0].u.s; uint32_t st = 0;
                for (uint32_t i = 0; i + d->len <= s->len; ) {
                    int m = memcmp(s->b + i, d->b, d->len) == 0;
                    if (m) { list_push(vm, r, v_strn(vm, s->b + st, i - st)); i += d->len; st = i; }
                    else i++;
                }
                list_push(vm, r, v_strn(vm, s->b + st, s->len - st));
            } else { err(vm, "ValueError: empty separator"); return v_none(); }
            return v_list(r);
        }
        if (strcmp(name, "join") == 0) {
            if (n != 1 || a[0].t != VT_LIST) { err(vm, "TypeError: join() needs a list"); return v_none(); }
            SB b; sb_init(&b, vm); List *l = a[0].u.l;
            for (uint32_t i = 0; i < l->len; i++) {
                if (i) sb_putn(&b, s->b, s->len);
                if (l->items[i].t != VT_STR) { err(vm, "TypeError: join() needs str elements"); return v_none(); }
                sb_putn(&b, l->items[i].u.s->b, l->items[i].u.s->len);
            }
            return v_strn(vm, b.b, b.len);
        }
        if (strcmp(name, "find") == 0) {
            if (n != 1 || a[0].t != VT_STR) { err(vm, "TypeError: find(sub)"); return v_none(); }
            Str *d = a[0].u.s; if (d->len == 0) return v_int(0);
            for (uint32_t i = 0; i + d->len <= s->len; i++) if (memcmp(s->b + i, d->b, d->len) == 0) return v_int(i);
            return v_int(-1);
        }
        if (strcmp(name, "replace") == 0) {
            if (n != 2 || a[0].t != VT_STR || a[1].t != VT_STR) { err(vm, "TypeError: replace(old, new)"); return v_none(); }
            Str *o = a[0].u.s, *nw = a[1].u.s; if (o->len == 0) return base;
            SB b; sb_init(&b, vm);
            for (uint32_t i = 0; i < s->len; ) {
                if (i + o->len <= s->len && memcmp(s->b + i, o->b, o->len) == 0) { sb_putn(&b, nw->b, nw->len); i += o->len; }
                else sb_putc(&b, s->b[i++]);
            }
            return v_strn(vm, b.b, b.len);
        }
        if (strcmp(name, "startswith") == 0) {
            if (n != 1 || a[0].t != VT_STR) { err(vm, "TypeError: startswith(sub)"); return v_none(); }
            Str *d = a[0].u.s; if (d->len > s->len) return v_bool(0);
            return v_bool(memcmp(s->b, d->b, d->len) == 0);
        }
        if (strcmp(name, "endswith") == 0) {
            if (n != 1 || a[0].t != VT_STR) { err(vm, "TypeError: endswith(sub)"); return v_none(); }
            Str *d = a[0].u.s; if (d->len > s->len) return v_bool(0);
            return v_bool(memcmp(s->b + (s->len - d->len), d->b, d->len) == 0);
        }
        err2(vm, "AttributeError: 'str' object has no method ", name); return v_none();
    }
    err2(vm, "AttributeError: object has no methods: ", type_name(base));
    return v_none();
}

/* ---------------------------------------------------------------------------
 *  default input reader (PS/2 keyboard, line-edited, echoed)
 * ------------------------------------------------------------------------- */
static int default_input(char *buf, int cap) {
    int len = 0;
    for (;;) {
        char c = kbd_getc();
        if (c == '\n') { kputc('\n'); buf[len] = 0; return len; }
        if (c == '\b') { if (len) { len--; kputc('\b'); } continue; }
        if ((unsigned char)c >= 32 && len < cap - 1) { buf[len++] = c; kputc(c); }
    }
}
static int (*g_input)(char *, int) = default_input;
void bpy_set_input(int (*reader)(char *buf, int cap)) { g_input = reader ? reader : default_input; }

/* ---------------------------------------------------------------------------
 *  public API
 * ------------------------------------------------------------------------- */
const char *bpy_version(void) { return BPY_VERSION; }

static void vm_init(bpy_vm *vm) {
    vm->arena = 0; vm->error = 0; vm->errmsg[0] = 0;
    vm->globals.names = 0; vm->globals.vals = 0; vm->globals.n = 0; vm->globals.cap = 0;
    vm->globals.gdecl = 0; vm->globals.ngdecl = 0;
    vm->cur = &vm->globals; vm->flow = F_NONE; vm->retval = v_none();
    vm->depth = 0; vm->input = g_input; vm->last = v_none(); vm->last_is_expr = 0;
}
static void vm_free_arena(bpy_vm *vm) {
    ablk *b = vm->arena;
    while (b) { ablk *nx = b->next; kfree(b); b = nx; }
    vm->arena = 0;
}

bpy_vm *bpy_new(void) {
    bpy_vm *vm = (bpy_vm *)kmalloc(sizeof(bpy_vm));
    if (!vm) return 0;
    vm_init(vm);
    return vm;
}
void bpy_free(bpy_vm *vm) {
    if (!vm) return;
    vm_free_arena(vm);
    kfree(vm);
}

int bpy_exec(bpy_vm *vm, const char *src, int echo) {
    vm->error = 0; vm->errmsg[0] = 0; vm->flow = F_NONE; vm->depth = 0;
    vm->cur = &vm->globals; vm->input = g_input; vm->last_is_expr = 0; vm->last = v_none();

    Tok *toks; int ntok;
    if (lex(vm, src, &toks, &ntok)) { kprintf("%s\n", vm->errmsg); return 1; }
    Node *prog = parse_program(vm, toks, ntok);
    if (vm->error) { kprintf("%s\n", vm->errmsg); return 1; }

    exec_block(vm, prog);
    if (vm->error) { kprintf("%s\n", vm->errmsg); return 1; }

    if (echo && vm->last_is_expr && vm->last.t != VT_NONE) {
        print_value(vm, vm->last, 1); kputc('\n');
    }
    return 0;
}

int bpy_run(const char *src) {
    bpy_vm vm; vm_init(&vm);
    int rc = bpy_exec(&vm, src, 0);
    vm_free_arena(&vm);
    return rc;
}
