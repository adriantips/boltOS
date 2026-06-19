/* ===========================================================================
 *  BoltCC  -  a freestanding C-family compiler + bytecode VM  (see boltcc.h)
 *
 *  Pipeline:  source -> lexer -> recursive-descent parser -> AST -> stack
 *  bytecode -> VM.  Three front-end dialects (C / C++ / C#) share one parser
 *  core; the differences are the entry-point name, the print intrinsics
 *  (printf vs std::cout vs Console.WriteLine) and which wrapper keywords
 *  (using / namespace / class / access modifiers) are skipped.
 *
 *  Values are 64-bit ints and strings only (no FPU in the kernel build).
 * ===========================================================================*/
#include <stdint.h>
#include "boltcc.h"
#include "string.h"
#include "kprintf.h"
#include "kheap.h"

/* ----------------------------------------------------------- diagnostics --- */
static void *g_errjmp[5];
static int   g_haderr;

/* ------------------------------------------------------------------ token --- */
enum {
    TK_EOF, TK_NUM, TK_STR, TK_ID,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
    TK_ASSIGN, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_NOT, TK_ANDAND, TK_OROR,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_INC, TK_DEC,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PCTEQ,
    TK_LP, TK_RP, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_SEMI, TK_COMMA, TK_DOT, TK_COLON, TK_SCOPE, TK_QUEST,
};

typedef struct { int kind; long ival; const char *str; int line; } Tok;

/* ------------------------------------------------------------------- AST ---- */
enum {
    N_NUM, N_STR, N_VAR, N_CALL, N_BIN, N_UN, N_ASSIGN, N_INDEX,
    N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_TERN, N_LOGAND, N_LOGOR,
    /* statements */
    N_BLOCK, N_VARDECL, N_DECLS, N_IF, N_WHILE, N_FOR, N_RET, N_BREAK, N_CONT, N_EXPRSTMT, N_EMPTY,
};

typedef struct Node Node;
struct Node {
    int   kind, op;
    long  ival;
    const char *str;        /* var/call name, string literal, static-ns flag   */
    int   isstatic;         /* N_VAR: a C# static namespace path (Console/Math) */
    Node *a, *b, *c, *d;    /* operands / sub-statements                        */
    Node **kids; int nkids; /* call args / block statements                     */
};

typedef struct {
    const char *name;
    const char **params; int nparams;
    Node *body;
    int   entry;            /* code pc, filled at codegen                       */
} Func;

/* --------------------------------------------------------------- bytecode --- */
enum {
    OP_PUSHI, OP_PUSHS, OP_LOADL, OP_STOREL, OP_LOADG, OP_STOREG,
    OP_POP, OP_DUP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_CALL, OP_RET,
    OP_PRINT, OP_PRINTLN, OP_PRINTF, OP_CSPRINT, OP_PUTC,
    OP_BUILTIN, OP_INDEX, OP_HALT,
};
enum { B_LEN, B_STR, B_INT, B_ABS, B_MIN, B_MAX, B_CHR, B_ORD };

typedef struct { int op; long a, b; const char *s; } Instr;

/* ------------------------------------------------------------- VM values --- */
typedef struct { int t; long i; char *s; } Val;   /* t: 0 int, 1 string */

/* --------------------------------------------------------------- context --- */
#define MAX_FUNCS    128
#define MAX_GLOBALS  256
#define MAX_LOCALS   96
#define MAX_LOOP     16

typedef struct {
    int lang;
    /* lexer */
    const char *src;
    Tok  *toks; int ntok, tokcap, ti;
    /* arenas */
    char *strarena; int strpos, strcap;
    char *astarena; int astpos, astcap;
    /* program */
    Func funcs[MAX_FUNCS]; int nfunc;
    const char *gname[MAX_GLOBALS]; Node *ginit[MAX_GLOBALS]; int nglob;
    /* codegen */
    Instr *code; int ncode, codecap;
    const char *loc[MAX_LOCALS]; int nloc, maxloc;
    struct { int contfix[64]; int ncont; int brk[64]; int nbrk; } loops[MAX_LOOP]; int nloop;
    int curfunc_nlocals;
} CC;

static CC *C;

static void cc_error(const char *msg, const char *detail) {
    kprintf("BoltCC: %s", msg);
    if (detail) { kprintf(": %s", detail); }
    kputc('\n');
    g_haderr = 1;
    __builtin_longjmp(g_errjmp, 1);
}

/* ------------------------------------------------------------- allocators -- */
static void *ast_alloc(int n) {
    n = (n + 7) & ~7;
    if (C->astpos + n > C->astcap) cc_error("program too large (AST arena full)", 0);
    void *p = C->astarena + C->astpos; C->astpos += n;
    return p;
}
static Node *node(int kind) {
    Node *x = (Node *)ast_alloc(sizeof(Node));
    memset(x, 0, sizeof(*x));
    x->kind = kind;
    return x;
}
static const char *intern(const char *s, int n) {
    if (C->strpos + n + 1 > C->strcap) cc_error("too many symbols (string arena full)", 0);
    char *d = C->strarena + C->strpos;
    memcpy(d, s, n); d[n] = 0; C->strpos += n + 1;
    return d;
}

/* ================================================================ LEXER ==== */
static int is_sp(char c)    { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static int is_dig(char c)   { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_dig(c); }

static void push_tok(int kind, long ival, const char *str, int line) {
    if (C->ntok >= C->tokcap) {
        int nc = C->tokcap ? C->tokcap * 2 : 256;
        Tok *nt = (Tok *)kmalloc((uint64_t)nc * sizeof(Tok));
        if (!nt) cc_error("out of memory (tokens)", 0);
        if (C->toks) { memcpy(nt, C->toks, (uint64_t)C->ntok * sizeof(Tok)); kfree(C->toks); }
        C->toks = nt; C->tokcap = nc;
    }
    Tok *t = &C->toks[C->ntok++];
    t->kind = kind; t->ival = ival; t->str = str; t->line = line;
}

static int esc_char(const char **p) {  /* *p points at the char after backslash */
    char c = **p; (*p)++;
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
    case '0': return '\0'; case '\\': return '\\'; case '\'': return '\'';
    case '"': return '"';  case 'a': return 7;    case 'b': return '\b';
    case 'f': return '\f'; case 'v': return '\v';
    default:  return (unsigned char)c;
    }
}

static void lex(void) {
    const char *p = C->src; int line = 1;
    while (*p) {
        char c = *p;
        if (c == '\n') { line++; p++; continue; }
        if (is_sp(c)) { p++; continue; }
        if (c == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (c == '/' && p[1] == '*') { p += 2; while (*p && !(*p == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; } if (*p) p += 2; continue; }
        if (c == '#') { while (*p && *p != '\n') p++; continue; }   /* preprocessor: skip line */

        if (is_dig(c)) {
            long v = 0;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (is_dig(*p) || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    int d = is_dig(*p) ? *p - '0' : (*p | 32) - 'a' + 10;
                    v = v * 16 + d; p++;
                }
            } else {
                while (is_dig(*p)) { v = v * 10 + (*p - '0'); p++; }
                if (*p == '.') cc_error("floating point is not supported (kernel has no FPU)", 0);
            }
            while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') p++;  /* int suffixes */
            push_tok(TK_NUM, v, 0, line); continue;
        }
        if (is_alpha(c)) {
            const char *s = p; while (is_alnum(*p)) p++;
            push_tok(TK_ID, 0, intern(s, (int)(p - s)), line); continue;
        }
        if (c == '"') {
            p++; char buf[1024]; int n = 0;
            while (*p && *p != '"') {
                int ch = (*p == '\\') ? (p++, esc_char(&p)) : (unsigned char)*p++;
                if (n < (int)sizeof(buf) - 1) buf[n++] = (char)ch;
            }
            if (*p == '"') p++;
            push_tok(TK_STR, 0, intern(buf, n), line); continue;
        }
        if (c == '\'') {
            p++; int v = (*p == '\\') ? (p++, esc_char(&p)) : (unsigned char)*p++;
            if (*p == '\'') p++;
            push_tok(TK_NUM, v, 0, line); continue;
        }
        /* operators / punctuation */
        #define TWO(a,b,k) if (c==a && p[1]==b) { push_tok(k,0,0,line); p+=2; continue; }
        TWO('=','=',TK_EQ) TWO('!','=',TK_NE) TWO('<','=',TK_LE) TWO('>','=',TK_GE)
        TWO('&','&',TK_ANDAND) TWO('|','|',TK_OROR) TWO('+','+',TK_INC) TWO('-','-',TK_DEC)
        TWO('+','=',TK_PLUSEQ) TWO('-','=',TK_MINUSEQ) TWO('*','=',TK_STAREQ)
        TWO('/','=',TK_SLASHEQ) TWO('%','=',TK_PCTEQ)
        TWO('<','<',TK_SHL) TWO('>','>',TK_SHR) TWO(':',':',TK_SCOPE)
        #undef TWO
        int k;
        switch (c) {
        case '+': k = TK_PLUS; break;   case '-': k = TK_MINUS; break;
        case '*': k = TK_STAR; break;   case '/': k = TK_SLASH; break;
        case '%': k = TK_PCT; break;    case '=': k = TK_ASSIGN; break;
        case '<': k = TK_LT; break;     case '>': k = TK_GT; break;
        case '!': k = TK_NOT; break;    case '&': k = TK_AMP; break;
        case '|': k = TK_PIPE; break;   case '^': k = TK_CARET; break;
        case '~': k = TK_TILDE; break;
        case '(': k = TK_LP; break;     case ')': k = TK_RP; break;
        case '{': k = TK_LBRACE; break; case '}': k = TK_RBRACE; break;
        case '[': k = TK_LBRACK; break; case ']': k = TK_RBRACK; break;
        case ';': k = TK_SEMI; break;   case ',': k = TK_COMMA; break;
        case '.': k = TK_DOT; break;    case ':': k = TK_COLON; break;
        case '?': k = TK_QUEST; break;
        default: cc_error("unexpected character", 0); return;
        }
        push_tok(k, 0, 0, line); p++;
    }
    push_tok(TK_EOF, 0, 0, line);

    /* collapse namespace qualifiers:  std::cout -> cout , a::b::c -> c */
    int w = 0;
    for (int r = 0; r < C->ntok; r++) {
        if (C->toks[r].kind == TK_ID && r + 1 < C->ntok && C->toks[r + 1].kind == TK_SCOPE) {
            r++;            /* drop the ID and the :: */
            continue;
        }
        C->toks[w++] = C->toks[r];
    }
    C->ntok = w;
}

/* =============================================================== PARSER ==== */
static Tok *cur(void)      { return &C->toks[C->ti]; }
static int  curk(void)     { return C->toks[C->ti].kind; }
static Tok *adv(void)      { return &C->toks[C->ti++]; }
static int  accept(int k)  { if (curk() == k) { C->ti++; return 1; } return 0; }
static void expect(int k, const char *what) { if (!accept(k)) cc_error("expected", what); }
static int  id_is(Tok *t, const char *s) { return t->kind == TK_ID && strcmp(t->str, s) == 0; }

static Node *parse_expr(void);
static Node *parse_assign(void);
static Node *parse_stmt(void);
static Node *parse_block(void);

static int is_type_kw(Tok *t) {
    if (t->kind != TK_ID) return 0;
    static const char *k[] = { "int","long","short","char","unsigned","signed","void",
                               "bool","float","double","string","var","auto","const",
                               "static","String","Int32","Int64",0 };
    for (int i = 0; k[i]; i++) if (strcmp(t->str, k[i]) == 0) return 1;
    return 0;
}

/* consume a (possibly compound) type specifier; returns nothing */
static void parse_type(void) {
    /* leading type keywords / a user type name */
    if (is_type_kw(cur())) { while (is_type_kw(cur())) adv(); }
    else if (curk() == TK_ID && (C->toks[C->ti + 1].kind == TK_ID ||
                                 C->toks[C->ti + 1].kind == TK_STAR ||
                                 C->toks[C->ti + 1].kind == TK_LT)) adv();
    /* template args  <...>  (balanced) */
    if (curk() == TK_LT) { int d = 0; do { if (curk()==TK_LT) d++; else if (curk()==TK_GT) d--; adv(); } while (d > 0 && curk() != TK_EOF); }
    while (curk() == TK_STAR || curk() == TK_AMP) adv();         /* pointers / refs */
    while (curk() == TK_LBRACK) { adv(); accept(TK_RBRACK); }    /* array []        */
}

/* arg list:  ( e , e , ... ) -> fills kids */
static void parse_args(Node *call) {
    expect(TK_LP, "(");
    Node *tmp[64]; int n = 0;
    if (curk() != TK_RP) {
        do { if (n < 64) tmp[n++] = parse_assign(); else parse_assign(); } while (accept(TK_COMMA));
    }
    expect(TK_RP, ")");
    call->nkids = n;
    call->kids = (Node **)ast_alloc(n * (int)sizeof(Node *));
    for (int i = 0; i < n; i++) call->kids[i] = tmp[i];
}

static Node *parse_primary(void) {
    Tok *t = cur();
    if (t->kind == TK_NUM) { adv(); Node *n = node(N_NUM); n->ival = t->ival; return n; }
    if (t->kind == TK_STR) { adv(); Node *n = node(N_STR); n->str = t->str; return n; }
    if (t->kind == TK_LP)  { adv(); Node *e = parse_expr(); expect(TK_RP, ")"); return e; }
    if (t->kind == TK_ID) {
        adv();
        /* C# static namespace path: Console.WriteLine, Math.Abs, System.Console.* */
        if (strcmp(t->str, "Console") == 0 || strcmp(t->str, "Math") == 0 ||
            strcmp(t->str, "Convert") == 0 || strcmp(t->str, "System") == 0) {
            char path[64]; int pl = 0;
            for (const char *s = t->str; *s && pl < 60; s++) path[pl++] = *s;
            while (curk() == TK_DOT) {
                adv(); if (curk() != TK_ID) break;
                path[pl++] = '.';
                for (const char *s = cur()->str; *s && pl < 60; s++) path[pl++] = *s;
                adv();
            }
            path[pl] = 0;
            Node *n = node(N_VAR); n->str = intern(path, pl); n->isstatic = 1;
            return n;
        }
        Node *n = node(N_VAR); n->str = t->str; return n;
    }
    cc_error("expected an expression", 0);
    return 0;
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    for (;;) {
        if (curk() == TK_LP) {                       /* call */
            Node *call = node(N_CALL);
            call->str = (e->kind == N_VAR) ? e->str : 0;
            if (!call->str) cc_error("call of non-function", 0);
            parse_args(call);
            e = call;
        } else if (curk() == TK_LBRACK) {            /* index */
            adv(); Node *idx = parse_expr(); expect(TK_RBRACK, "]");
            Node *n = node(N_INDEX); n->a = e; n->b = idx; e = n;
        } else if (curk() == TK_DOT) {               /* member: .Length .length() .ToString() */
            adv();
            if (curk() != TK_ID) cc_error("expected member name", 0);
            const char *m = cur()->str; adv();
            Node *call = node(N_CALL);
            if (strcmp(m, "Length") == 0)        call->str = "len";
            else if (strcmp(m, "length") == 0)   { call->str = "len"; if (curk()==TK_LP){adv();accept(TK_RP);} }
            else if (strcmp(m, "ToString") == 0) { call->str = "str"; if (curk()==TK_LP){adv();accept(TK_RP);} }
            else cc_error("unsupported member", m);
            call->nkids = 1; call->kids = (Node **)ast_alloc(sizeof(Node *)); call->kids[0] = e;
            e = call;
        } else if (curk() == TK_INC) { adv(); Node *n = node(N_POSTINC); n->a = e; e = n; }
        else if (curk() == TK_DEC)   { adv(); Node *n = node(N_POSTDEC); n->a = e; e = n; }
        else break;
    }
    return e;
}

static Node *parse_unary(void) {
    int k = curk();
    if (k == TK_MINUS) { adv(); Node *n = node(N_UN); n->op = OP_NEG; n->a = parse_unary(); return n; }
    if (k == TK_NOT)   { adv(); Node *n = node(N_UN); n->op = OP_NOT; n->a = parse_unary(); return n; }
    if (k == TK_PLUS)  { adv(); return parse_unary(); }
    if (k == TK_TILDE) { adv(); Node *n = node(N_UN); n->op = OP_BXOR; n->a = parse_unary(); return n; }
    if (k == TK_INC)   { adv(); Node *n = node(N_PREINC); n->a = parse_unary(); return n; }
    if (k == TK_DEC)   { adv(); Node *n = node(N_PREDEC); n->a = parse_unary(); return n; }
    return parse_postfix();
}

/* precedence-climbing for binary operators */
static int bin_prec(int k, int *op) {
    switch (k) {
    case TK_STAR:  *op = OP_MUL; return 11; case TK_SLASH: *op = OP_DIV; return 11;
    case TK_PCT:   *op = OP_MOD; return 11;
    case TK_PLUS:  *op = OP_ADD; return 10; case TK_MINUS: *op = OP_SUB; return 10;
    case TK_SHL:   *op = OP_SHL; return 9;  case TK_SHR:   *op = OP_SHR; return 9;
    case TK_LT:    *op = OP_LT;  return 8;  case TK_LE:    *op = OP_LE;  return 8;
    case TK_GT:    *op = OP_GT;  return 8;  case TK_GE:    *op = OP_GE;  return 8;
    case TK_EQ:    *op = OP_EQ;  return 7;  case TK_NE:    *op = OP_NE;  return 7;
    case TK_AMP:   *op = OP_BAND;return 6;
    case TK_CARET: *op = OP_BXOR;return 5;
    case TK_PIPE:  *op = OP_BOR; return 4;
    default: return -1;
    }
}
static Node *parse_binexpr(int minp) {
    Node *lhs = parse_unary();
    for (;;) {
        int op, p = bin_prec(curk(), &op);
        if (p < minp || p < 0) break;
        adv();
        Node *rhs = parse_binexpr(p + 1);
        Node *n = node(N_BIN); n->op = op; n->a = lhs; n->b = rhs; lhs = n;
    }
    return lhs;
}
static Node *parse_logand(void) {
    Node *l = parse_binexpr(0);
    while (curk() == TK_ANDAND) { adv(); Node *n = node(N_LOGAND); n->a = l; n->b = parse_binexpr(0); l = n; }
    return l;
}
static Node *parse_logor(void) {
    Node *l = parse_logand();
    while (curk() == TK_OROR) { adv(); Node *n = node(N_LOGOR); n->a = l; n->b = parse_logand(); l = n; }
    return l;
}
static Node *parse_ternary(void) {
    Node *c = parse_logor();
    if (accept(TK_QUEST)) {
        Node *n = node(N_TERN); n->a = c; n->b = parse_assign();
        expect(TK_COLON, ":"); n->c = parse_assign();
        return n;
    }
    return c;
}
static Node *parse_assign(void) {
    Node *l = parse_ternary();
    int k = curk(), op = -1;
    switch (k) {
    case TK_ASSIGN:  op = 0; break;
    case TK_PLUSEQ:  op = OP_ADD; break; case TK_MINUSEQ: op = OP_SUB; break;
    case TK_STAREQ:  op = OP_MUL; break; case TK_SLASHEQ: op = OP_DIV; break;
    case TK_PCTEQ:   op = OP_MOD; break;
    default: return l;
    }
    if (l->kind != N_VAR) cc_error("assignment to non-variable", 0);
    adv();
    Node *n = node(N_ASSIGN); n->op = op; n->str = l->str; n->a = parse_assign();
    return n;
}
static Node *parse_expr(void) { return parse_assign(); }

/* C++ cout chain handled at statement scope:  cout << a << b << endl;  */
static Node *parse_cout(void) {
    adv();                                  /* 'cout' */
    Node *blk = node(N_BLOCK);
    Node *tmp[64]; int n = 0;
    while (accept(TK_SHL)) {
        Node *item;
        if (id_is(cur(), "endl")) { adv(); item = node(N_STR); item->str = "\n"; }
        else item = parse_binexpr(10);      /* additive+ : do NOT consume the '<<' chain */
        Node *call = node(N_CALL); call->str = "print";
        call->nkids = 1; call->kids = (Node **)ast_alloc(sizeof(Node *)); call->kids[0] = item;
        Node *es = node(N_EXPRSTMT); es->a = call;
        if (n < 64) tmp[n++] = es;
    }
    expect(TK_SEMI, ";");
    blk->nkids = n; blk->kids = (Node **)ast_alloc(n * (int)sizeof(Node *));
    for (int i = 0; i < n; i++) blk->kids[i] = tmp[i];
    return blk;
}

/* a declaration begins a statement when it looks like `type name` */
static int looks_like_decl(void) {
    if (is_type_kw(cur())) return 1;
    if (curk() == TK_ID && C->toks[C->ti + 1].kind == TK_ID) return 1;   /* UserType v */
    return 0;
}

static Node *parse_vardecl(void) {       /* one or more: type a [=e] (, b [=e])* ; */
    parse_type();
    Node *blk = node(N_DECLS);
    Node *tmp[32]; int n = 0;
    do {
        if (curk() != TK_ID) cc_error("expected variable name", 0);
        const char *nm = cur()->str; adv();
        while (curk() == TK_LBRACK) { adv(); accept(TK_RBRACK); }   /* array decl */
        Node *d = node(N_VARDECL); d->str = nm;
        if (accept(TK_ASSIGN)) d->a = parse_assign();
        if (n < 32) tmp[n++] = d;
    } while (accept(TK_COMMA));
    expect(TK_SEMI, ";");
    blk->nkids = n; blk->kids = (Node **)ast_alloc(n * (int)sizeof(Node *));
    for (int i = 0; i < n; i++) blk->kids[i] = tmp[i];
    return blk;
}

static Node *parse_stmt(void) {
    if (curk() == TK_LBRACE) return parse_block();
    if (curk() == TK_SEMI)   { adv(); return node(N_EMPTY); }

    if (id_is(cur(), "if")) {
        adv(); expect(TK_LP, "("); Node *n = node(N_IF); n->a = parse_expr(); expect(TK_RP, ")");
        n->b = parse_stmt();
        if (id_is(cur(), "else")) { adv(); n->c = parse_stmt(); }
        return n;
    }
    if (id_is(cur(), "while")) {
        adv(); expect(TK_LP, "("); Node *n = node(N_WHILE); n->a = parse_expr(); expect(TK_RP, ")");
        n->b = parse_stmt(); return n;
    }
    if (id_is(cur(), "for")) {
        adv(); expect(TK_LP, "(");
        Node *n = node(N_FOR);
        if (curk() != TK_SEMI) {
            if (looks_like_decl()) n->a = parse_vardecl();           /* consumes its ';' */
            else { Node *e = node(N_EXPRSTMT); e->a = parse_expr(); expect(TK_SEMI, ";"); n->a = e; }
        } else adv();
        if (curk() != TK_SEMI) n->b = parse_expr();
        expect(TK_SEMI, ";");
        if (curk() != TK_RP) n->c = parse_expr();
        expect(TK_RP, ")");
        n->d = parse_stmt();
        return n;
    }
    if (id_is(cur(), "return")) {
        adv(); Node *n = node(N_RET);
        if (curk() != TK_SEMI) n->a = parse_expr();
        expect(TK_SEMI, ";"); return n;
    }
    if (id_is(cur(), "break"))    { adv(); expect(TK_SEMI, ";"); return node(N_BREAK); }
    if (id_is(cur(), "continue")) { adv(); expect(TK_SEMI, ";"); return node(N_CONT); }
    if (C->lang == BCC_CPP && id_is(cur(), "cout")) return parse_cout();
    if (id_is(cur(), "cin")) cc_error("cin / input is not supported", 0);

    if (looks_like_decl()) return parse_vardecl();

    Node *e = node(N_EXPRSTMT); e->a = parse_expr(); expect(TK_SEMI, ";");
    return e;
}

static Node *parse_block(void) {
    expect(TK_LBRACE, "{");
    Node *blk = node(N_BLOCK);
    Node *tmp[512]; int n = 0;
    while (curk() != TK_RBRACE && curk() != TK_EOF) {
        Node *s = parse_stmt();
        if (n < 512) tmp[n++] = s;
    }
    expect(TK_RBRACE, "}");
    blk->nkids = n; blk->kids = (Node **)ast_alloc(n * (int)sizeof(Node *));
    for (int i = 0; i < n; i++) blk->kids[i] = tmp[i];
    return blk;
}

/* skip the wrapper keywords C#/C++ put around top-level code */
static int skip_top_noise(void) {  /* returns 1 if it consumed something */
    if (id_is(cur(), "using")) { while (curk() != TK_SEMI && curk() != TK_EOF) adv(); accept(TK_SEMI); return 1; }
    if (id_is(cur(), "namespace")) { adv(); while (curk()!=TK_LBRACE && curk()!=TK_EOF) adv(); accept(TK_LBRACE); return 1; }
    if (id_is(cur(), "class") || id_is(cur(), "struct")) {
        adv(); while (curk()!=TK_LBRACE && curk()!=TK_EOF) adv(); accept(TK_LBRACE); return 1;
    }
    static const char *mods[] = { "public","private","protected","internal","static",
                                  "sealed","abstract","virtual","override","partial",
                                  "const","readonly","extern","inline","unsafe",0 };
    for (int i = 0; mods[i]; i++) if (id_is(cur(), mods[i])) { adv(); return 1; }
    if (curk() == TK_RBRACE) { adv(); return 1; }   /* close of namespace/class */
    if (curk() == TK_SEMI)   { adv(); return 1; }
    return 0;
}

static void parse_func_or_global(void) {
    parse_type();
    if (curk() != TK_ID) cc_error("expected a name at top level", 0);
    const char *nm = cur()->str; adv();

    if (curk() == TK_LP) {                              /* function */
        if (C->nfunc >= MAX_FUNCS) cc_error("too many functions", 0);
        Func *f = &C->funcs[C->nfunc++];
        f->name = nm;
        const char *ps[32]; int np = 0;
        expect(TK_LP, "(");
        if (curk() != TK_RP) {
            do {
                if (id_is(cur(), "void") && C->toks[C->ti + 1].kind == TK_RP) { adv(); break; }
                parse_type();
                if (curk() == TK_ID) { if (np < 32) ps[np++] = cur()->str; adv(); }
                while (curk() == TK_LBRACK) { adv(); accept(TK_RBRACK); }
            } while (accept(TK_COMMA));
        }
        expect(TK_RP, ")");
        f->nparams = np;
        f->params = (const char **)ast_alloc(np * (int)sizeof(char *));
        for (int i = 0; i < np; i++) f->params[i] = ps[i];
        f->body = parse_block();
    } else {                                            /* global variable(s) */
        for (;;) {
            while (curk() == TK_LBRACK) { adv(); accept(TK_RBRACK); }
            if (C->nglob >= MAX_GLOBALS) cc_error("too many globals", 0);
            int gi = C->nglob++;
            C->gname[gi] = nm;
            C->ginit[gi] = accept(TK_ASSIGN) ? parse_assign() : 0;
            if (!accept(TK_COMMA)) break;
            if (curk() != TK_ID) cc_error("expected variable name", 0);
            nm = cur()->str; adv();
        }
        expect(TK_SEMI, ";");
    }
}

static void parse_program(void) {
    while (curk() != TK_EOF) {
        if (skip_top_noise()) continue;
        parse_func_or_global();
    }
}

/* =============================================================== CODEGEN ==== */
static int emit(int op, long a, long b, const char *s) {
    if (C->ncode >= C->codecap) {
        int nc = C->codecap ? C->codecap * 2 : 512;
        Instr *ni = (Instr *)kmalloc((uint64_t)nc * sizeof(Instr));
        if (!ni) cc_error("out of memory (code)", 0);
        if (C->code) { memcpy(ni, C->code, (uint64_t)C->ncode * sizeof(Instr)); kfree(C->code); }
        C->code = ni; C->codecap = nc;
    }
    Instr *i = &C->code[C->ncode];
    i->op = op; i->a = a; i->b = b; i->s = s;
    return C->ncode++;
}
static int loc_find(const char *nm) {
    for (int i = C->nloc - 1; i >= 0; i--) if (strcmp(C->loc[i], nm) == 0) return i;
    return -1;
}
static int loc_add(const char *nm) {
    if (C->nloc >= MAX_LOCALS) cc_error("too many locals", 0);
    int i = C->nloc++;
    C->loc[i] = nm;
    if (C->nloc > C->maxloc) C->maxloc = C->nloc;
    return i;
}
static int glob_find(const char *nm) {
    for (int i = 0; i < C->nglob; i++) if (strcmp(C->gname[i], nm) == 0) return i;
    return -1;
}
static int func_find(const char *nm) {
    for (int i = 0; i < C->nfunc; i++) if (strcmp(C->funcs[i].name, nm) == 0) return i;
    return -1;
}

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

static void gen_load(const char *nm) {
    int l = loc_find(nm); if (l >= 0) { emit(OP_LOADL, l, 0, 0); return; }
    int g = glob_find(nm); if (g >= 0) { emit(OP_LOADG, g, 0, 0); return; }
    cc_error("undefined variable", nm);
}
static void gen_store(const char *nm) {
    int l = loc_find(nm); if (l >= 0) { emit(OP_STOREL, l, 0, 0); return; }
    int g = glob_find(nm); if (g >= 0) { emit(OP_STOREG, g, 0, 0); return; }
    cc_error("undefined variable", nm);
}

static int builtin_id(const char *nm) {
    if (!strcmp(nm,"len")) return B_LEN;  if (!strcmp(nm,"str")) return B_STR;
    if (!strcmp(nm,"int")) return B_INT;  if (!strcmp(nm,"abs")) return B_ABS;
    if (!strcmp(nm,"min")) return B_MIN;  if (!strcmp(nm,"max")) return B_MAX;
    if (!strcmp(nm,"chr")) return B_CHR;  if (!strcmp(nm,"ord")) return B_ORD;
    return -1;
}

/* normalise a C# static path to a simple intrinsic name */
static const char *normalize_call(const char *nm) {
    if (!strncmp(nm, "System.", 7)) nm += 7;
    if (!strcmp(nm, "Console.WriteLine")) return "__wl";
    if (!strcmp(nm, "Console.Write"))     return "__w";
    if (!strcmp(nm, "Math.Abs")) return "abs";
    if (!strcmp(nm, "Math.Max")) return "max";
    if (!strcmp(nm, "Math.Min")) return "min";
    if (!strcmp(nm, "Convert.ToInt32")) return "int";
    if (!strcmp(nm, "Convert.ToString")) return "str";
    return nm;
}

static void gen_call(Node *n) {
    const char *nm = normalize_call(n->str);
    int argc = n->nkids;

    if (!strcmp(nm, "printf")) {
        for (int i = 0; i < argc; i++) gen_expr(n->kids[i]);
        emit(OP_PRINTF, 0, argc, 0); return;
    }
    if (!strcmp(nm, "__wl") || !strcmp(nm, "__w")) {
        for (int i = 0; i < argc; i++) gen_expr(n->kids[i]);
        emit(OP_CSPRINT, !strcmp(nm, "__wl") ? 1 : 0, argc, 0); return;
    }
    if (!strcmp(nm, "print")) {
        if (argc != 1) cc_error("print expects one argument", 0);
        gen_expr(n->kids[0]); emit(OP_PRINT, 0, 0, 0); return;
    }
    if (!strcmp(nm, "println")) {
        if (argc == 0) { emit(OP_PUSHS, 0, 0, ""); emit(OP_PRINTLN, 0, 0, 0); return; }
        gen_expr(n->kids[0]); emit(OP_PRINTLN, 0, 0, 0); return;
    }
    if (!strcmp(nm, "puts")) {
        if (argc != 1) cc_error("puts expects one argument", 0);
        gen_expr(n->kids[0]); emit(OP_PRINTLN, 0, 0, 0); return;
    }
    if (!strcmp(nm, "putchar")) {
        if (argc != 1) cc_error("putchar expects one argument", 0);
        gen_expr(n->kids[0]); emit(OP_PUTC, 0, 0, 0); return;
    }
    int b = builtin_id(nm);
    if (b >= 0) {
        for (int i = 0; i < argc; i++) gen_expr(n->kids[i]);
        emit(OP_BUILTIN, b, argc, 0); return;
    }
    int fi = func_find(nm);
    if (fi < 0) cc_error("undefined function", nm);
    if (argc != C->funcs[fi].nparams) cc_error("wrong argument count for", nm);
    for (int i = 0; i < argc; i++) gen_expr(n->kids[i]);
    emit(OP_CALL, fi, argc, 0);
}

static void gen_expr(Node *n) {
    switch (n->kind) {
    case N_NUM: emit(OP_PUSHI, n->ival, 0, 0); break;
    case N_STR: emit(OP_PUSHS, 0, 0, n->str); break;
    case N_VAR:
        if (n->isstatic) cc_error("cannot use as a value", n->str);
        gen_load(n->str); break;
    case N_CALL: gen_call(n); break;
    case N_INDEX: gen_expr(n->a); gen_expr(n->b); emit(OP_INDEX, 0, 0, 0); break;
    case N_BIN:
        gen_expr(n->a); gen_expr(n->b); emit(n->op, 0, 0, 0); break;
    case N_UN:
        if (n->op == OP_BXOR) { gen_expr(n->a); emit(OP_PUSHI, -1, 0, 0); emit(OP_BXOR, 0, 0, 0); }
        else { gen_expr(n->a); emit(n->op, 0, 0, 0); }
        break;
    case N_LOGAND: {
        gen_expr(n->a); int j1 = emit(OP_JZ, 0, 0, 0);
        gen_expr(n->b); int j2 = emit(OP_JZ, 0, 0, 0);
        emit(OP_PUSHI, 1, 0, 0); int j3 = emit(OP_JMP, 0, 0, 0);
        C->code[j1].a = C->ncode; C->code[j2].a = C->ncode;
        emit(OP_PUSHI, 0, 0, 0); C->code[j3].a = C->ncode;
        break; }
    case N_LOGOR: {
        gen_expr(n->a); int j1 = emit(OP_JNZ, 0, 0, 0);
        gen_expr(n->b); int j2 = emit(OP_JNZ, 0, 0, 0);
        emit(OP_PUSHI, 0, 0, 0); int j3 = emit(OP_JMP, 0, 0, 0);
        C->code[j1].a = C->ncode; C->code[j2].a = C->ncode;
        emit(OP_PUSHI, 1, 0, 0); C->code[j3].a = C->ncode;
        break; }
    case N_TERN: {
        gen_expr(n->a); int je = emit(OP_JZ, 0, 0, 0);
        gen_expr(n->b); int jd = emit(OP_JMP, 0, 0, 0);
        C->code[je].a = C->ncode; gen_expr(n->c); C->code[jd].a = C->ncode;
        break; }
    case N_ASSIGN:
        if (n->op == 0) { gen_expr(n->a); emit(OP_DUP, 0, 0, 0); gen_store(n->str); }
        else { gen_load(n->str); gen_expr(n->a); emit(n->op, 0, 0, 0); emit(OP_DUP, 0, 0, 0); gen_store(n->str); }
        break;
    case N_PREINC: case N_PREDEC: {
        const char *nm = n->a->str;
        gen_load(nm); emit(OP_PUSHI, 1, 0, 0); emit(n->kind == N_PREINC ? OP_ADD : OP_SUB, 0, 0, 0);
        emit(OP_DUP, 0, 0, 0); gen_store(nm); break; }
    case N_POSTINC: case N_POSTDEC: {
        const char *nm = n->a->str;
        gen_load(nm); gen_load(nm); emit(OP_PUSHI, 1, 0, 0);
        emit(n->kind == N_POSTINC ? OP_ADD : OP_SUB, 0, 0, 0); gen_store(nm); break; }
    default: cc_error("internal: bad expr node", 0);
    }
}

static void gen_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case N_EMPTY: break;
    case N_BLOCK: {
        int save = C->nloc;
        for (int i = 0; i < n->nkids; i++) gen_stmt(n->kids[i]);
        C->nloc = save; break; }
    case N_DECLS:    /* declarations do NOT open a scope (stay live in the block) */
        for (int i = 0; i < n->nkids; i++) gen_stmt(n->kids[i]);
        break;
    case N_VARDECL: {
        int idx = loc_add(n->str);
        if (n->a) { gen_expr(n->a); emit(OP_STOREL, idx, 0, 0); }
        break; }
    case N_EXPRSTMT: gen_expr(n->a); emit(OP_POP, 0, 0, 0); break;
    case N_IF: {
        gen_expr(n->a); int je = emit(OP_JZ, 0, 0, 0);
        gen_stmt(n->b);
        if (n->c) { int jd = emit(OP_JMP, 0, 0, 0); C->code[je].a = C->ncode; gen_stmt(n->c); C->code[jd].a = C->ncode; }
        else C->code[je].a = C->ncode;
        break; }
    case N_WHILE: {
        if (C->nloop >= MAX_LOOP) cc_error("loops nested too deeply", 0);
        int top = C->ncode;
        gen_expr(n->a); int je = emit(OP_JZ, 0, 0, 0);
        C->loops[C->nloop].nbrk = 0; C->loops[C->nloop].ncont = 0; C->nloop++;
        gen_stmt(n->b);
        emit(OP_JMP, top, 0, 0);
        int end = C->ncode;
        C->code[je].a = end;
        C->nloop--;
        for (int i = 0; i < C->loops[C->nloop].ncont; i++) C->code[C->loops[C->nloop].contfix[i]].a = top;
        for (int i = 0; i < C->loops[C->nloop].nbrk;  i++) C->code[C->loops[C->nloop].brk[i]].a    = end;
        break; }
    case N_FOR: {
        if (C->nloop >= MAX_LOOP) cc_error("loops nested too deeply", 0);
        int save = C->nloc;
        if (n->a) gen_stmt(n->a);
        int top = C->ncode;
        int je = -1;
        if (n->b) { gen_expr(n->b); je = emit(OP_JZ, 0, 0, 0); }
        C->loops[C->nloop].nbrk = 0; C->loops[C->nloop].ncont = 0; C->nloop++;
        gen_stmt(n->d);
        int contpc = C->ncode;                       /* continue -> the post step */
        if (n->c) { gen_expr(n->c); emit(OP_POP, 0, 0, 0); }
        emit(OP_JMP, top, 0, 0);
        int end = C->ncode;
        if (je >= 0) C->code[je].a = end;
        C->nloop--;
        for (int i = 0; i < C->loops[C->nloop].ncont; i++) C->code[C->loops[C->nloop].contfix[i]].a = contpc;
        for (int i = 0; i < C->loops[C->nloop].nbrk;  i++) C->code[C->loops[C->nloop].brk[i]].a     = end;
        C->nloc = save; break; }
    case N_RET:
        if (n->a) gen_expr(n->a); else emit(OP_PUSHI, 0, 0, 0);
        emit(OP_RET, 0, 0, 0); break;
    case N_BREAK: {
        if (C->nloop == 0) cc_error("break outside loop", 0);
        int j = emit(OP_JMP, 0, 0, 0);
        C->loops[C->nloop - 1].brk[C->loops[C->nloop - 1].nbrk++] = j; break; }
    case N_CONT: {
        if (C->nloop == 0) cc_error("continue outside loop", 0);
        int j = emit(OP_JMP, 0, 0, 0);
        C->loops[C->nloop - 1].contfix[C->loops[C->nloop - 1].ncont++] = j; break; }
    default: cc_error("internal: bad stmt node", 0);
    }
}

/* =============================================================== runtime ==== */
static Val   g_globstore[MAX_GLOBALS];
static char *g_rt; static int g_rtpos, g_rtcap;

static char *rt_alloc(int n) {
    if (g_rtpos + n > g_rtcap) cc_error("runtime: string memory exhausted", 0);
    char *p = g_rt + g_rtpos; g_rtpos += n; return p;
}
static char *itoa_rt(long v) {
    char tmp[24]; int n = 0; uint64_t u; int neg = 0;
    if (v < 0) { neg = 1; u = (uint64_t)(-(v + 1)) + 1; } else u = (uint64_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) tmp[n++] = '-';
    char *out = rt_alloc(n + 1);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0; return out;
}
static const char *val_cstr(Val v) {        /* read-only view for printing */
    return v.t == 1 ? (v.s ? v.s : "") : itoa_rt(v.i);
}

/* ================================================================== VM ===== */
typedef struct { int retpc; Val locals[MAX_LOCALS]; } Frame;

static void print_cstr(const char *s) { while (*s) kputc(*s++); }

static void do_printf(Val *args, int argc) {
    if (argc == 0) return;
    const char *f = args[0].t == 1 ? args[0].s : "";
    int ai = 1;
    for (const char *p = f; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        p++;
        if (*p == '%') { kputc('%'); continue; }
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') p++;  /* flags */
        while (is_dig(*p)) p++;                                                      /* width */
        if (*p == '.') { p++; while (is_dig(*p)) p++; }                              /* prec  */
        while (*p == 'l' || *p == 'h' || *p == 'z') p++;                             /* len   */
        Val a = (ai < argc) ? args[ai++] : (Val){0,0,0};
        switch (*p) {
        case 'd': case 'i': case 'u': print_cstr(itoa_rt(a.t == 1 ? 0 : a.i)); break;
        case 'c': kputc((char)(a.t == 1 ? (a.s ? a.s[0] : 0) : a.i)); break;
        case 's': print_cstr(val_cstr(a)); break;
        case 'x': case 'X': case 'p': {
            char tmp[20]; int n = 0; uint64_t u = (uint64_t)(a.t == 1 ? 0 : a.i);
            const char *hd = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            if (!u) tmp[n++] = '0'; while (u) { tmp[n++] = hd[u & 15]; u >>= 4; }
            for (int i = n - 1; i >= 0; i--) kputc(tmp[i]); break; }
        default: kputc('%'); if (*p) kputc(*p); break;
        }
    }
}

/* .NET-style Console.Write/WriteLine: {0} {1} substitution, else concatenation */
static void do_csprint(Val *args, int argc, int nl) {
    if (argc == 0) { if (nl) kputc('\n'); return; }
    if (argc >= 2 && args[0].t == 1 && strchr(args[0].s, '{')) {
        for (const char *p = args[0].s; *p; p++) {
            if (*p == '{' && is_dig(p[1])) {
                int idx = 0; p++;
                while (is_dig(*p)) { idx = idx * 10 + (*p - '0'); p++; }
                while (*p && *p != '}') p++;            /* skip :format */
                int ai = idx + 1;
                if (ai < argc) print_cstr(val_cstr(args[ai]));
            } else if (*p == '{' && p[1] == '{') { kputc('{'); p++; }
            else if (*p == '}' && p[1] == '}') { kputc('}'); p++; }
            else kputc(*p);
        }
    } else {
        for (int i = 0; i < argc; i++) print_cstr(val_cstr(args[i]));
    }
    if (nl) kputc('\n');
}

static Val concat(Val a, Val b) {
    const char *sa = val_cstr(a), *sb = val_cstr(b);
    int la = (int)strlen(sa), lb = (int)strlen(sb);
    char *r = rt_alloc(la + lb + 1);
    memcpy(r, sa, la); memcpy(r + la, sb, lb); r[la + lb] = 0;
    Val v = { 1, 0, r }; return v;
}
static long need_int(Val v, const char *what) { if (v.t != 0) cc_error("runtime: expected an integer in", what); return v.i; }

static Val   *g_vm_st;
static Frame *g_vm_frames;

static void vm_run(int start_ip) {
    Val *st = g_vm_st = (Val *)kmalloc(4096 * sizeof(Val));
    Frame *frames = g_vm_frames = (Frame *)kmalloc(512 * sizeof(Frame));
    if (!st || !frames) cc_error("out of memory (VM)", 0);
    int sp = 0, fp = 0;
    frames[0].retpc = -1;
    for (int i = 0; i < MAX_LOCALS; i++) frames[0].locals[i] = (Val){0,0,0};

    int ip = start_ip;
    uint64_t guard = 0;
    #define PUSH(v) do { if (sp >= 4096) cc_error("runtime: stack overflow", 0); st[sp++] = (v); } while (0)
    #define POP()   (st[--sp])
    #define VI(x)   ((Val){0,(long)(x),0})

    for (;;) {
        if (++guard > 800000000ull) cc_error("runtime: execution limit reached (infinite loop?)", 0);
        Instr *in = &C->code[ip++];
        switch (in->op) {
        case OP_PUSHI: PUSH(VI(in->a)); break;
        case OP_PUSHS: { Val v = { 1, 0, (char *)in->s }; PUSH(v); break; }
        case OP_LOADL: PUSH(frames[fp].locals[in->a]); break;
        case OP_STOREL: frames[fp].locals[in->a] = POP(); break;
        case OP_LOADG: PUSH(g_globstore[in->a]); break;
        case OP_STOREG: g_globstore[in->a] = POP(); break;
        case OP_POP: sp--; break;
        case OP_DUP: { Val v = st[sp - 1]; PUSH(v); break; }
        case OP_ADD: { Val b = POP(), a = POP(); PUSH((a.t == 1 || b.t == 1) ? concat(a, b) : VI(a.i + b.i)); break; }
        case OP_SUB: { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"-") - need_int(b,"-"))); break; }
        case OP_MUL: { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"*") * need_int(b,"*"))); break; }
        case OP_DIV: { Val b = POP(), a = POP(); long d = need_int(b,"/"); if (!d) cc_error("runtime: divide by zero", 0); PUSH(VI(need_int(a,"/") / d)); break; }
        case OP_MOD: { Val b = POP(), a = POP(); long d = need_int(b,"%"); if (!d) cc_error("runtime: modulo by zero", 0); PUSH(VI(need_int(a,"%") % d)); break; }
        case OP_NEG: { Val a = POP(); PUSH(VI(-need_int(a,"unary -"))); break; }
        case OP_BAND: { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"&") & need_int(b,"&"))); break; }
        case OP_BOR:  { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"|") | need_int(b,"|"))); break; }
        case OP_BXOR: { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"^") ^ need_int(b,"^"))); break; }
        case OP_SHL:  { Val b = POP(), a = POP(); PUSH(VI(need_int(a,"<<") << need_int(b,"<<"))); break; }
        case OP_SHR:  { Val b = POP(), a = POP(); PUSH(VI(need_int(a,">>") >> need_int(b,">>"))); break; }
        case OP_NOT:  { Val a = POP(); PUSH(VI(a.t == 1 ? 0 : !a.i)); break; }
        case OP_EQ: case OP_NE: {
            Val b = POP(), a = POP(); int eq;
            if (a.t == 1 && b.t == 1) eq = strcmp(a.s ? a.s : "", b.s ? b.s : "") == 0;
            else if (a.t == 0 && b.t == 0) eq = a.i == b.i;
            else eq = 0;
            PUSH(VI(in->op == OP_EQ ? eq : !eq)); break; }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            Val b = POP(), a = POP(); long r;
            if (a.t == 1 && b.t == 1) r = strcmp(a.s ? a.s : "", b.s ? b.s : "");
            else r = need_int(a,"comparison") - need_int(b,"comparison");
            int res = in->op==OP_LT ? r<0 : in->op==OP_LE ? r<=0 : in->op==OP_GT ? r>0 : r>=0;
            PUSH(VI(res)); break; }
        case OP_JMP: ip = (int)in->a; break;
        case OP_JZ: { Val a = POP(); if (a.t == 1 ? (a.s == 0 || a.s[0] == 0) : a.i == 0) ip = (int)in->a; break; }
        case OP_JNZ:{ Val a = POP(); if (a.t == 1 ? (a.s && a.s[0]) : a.i != 0) ip = (int)in->a; break; }
        case OP_CALL: {
            int fi = (int)in->a, argc = (int)in->b;
            if (fp + 1 >= 512) cc_error("runtime: call stack overflow (deep recursion?)", 0);
            Frame *nf = &frames[fp + 1];
            for (int i = 0; i < MAX_LOCALS; i++) nf->locals[i] = (Val){0,0,0};
            for (int i = 0; i < argc; i++) nf->locals[argc - 1 - i] = POP();
            nf->retpc = ip; fp++; ip = C->funcs[fi].entry; break; }
        case OP_RET: { Val rv = POP(); int rp = frames[fp].retpc; fp--; PUSH(rv); if (rp < 0) { ip = -1; } else ip = rp; break; }
        case OP_PRINT:   { Val a = POP(); print_cstr(val_cstr(a)); PUSH(VI(0)); break; }
        case OP_PRINTLN: { Val a = POP(); print_cstr(val_cstr(a)); kputc('\n'); PUSH(VI(0)); break; }
        case OP_PUTC:    { Val a = POP(); kputc((char)(a.t==1 ? (a.s?a.s[0]:0) : a.i)); PUSH(VI(0)); break; }
        case OP_PRINTF:  { int argc = (int)in->b; do_printf(&st[sp - argc], argc); sp -= argc; PUSH(VI(0)); break; }
        case OP_CSPRINT: { int argc = (int)in->b; do_csprint(&st[sp - argc], argc, (int)in->a); sp -= argc; PUSH(VI(0)); break; }
        case OP_INDEX: { Val idx = POP(), s = POP(); if (s.t != 1) cc_error("runtime: indexing a non-string", 0);
                         long i = need_int(idx,"index"); int n = (int)strlen(s.s ? s.s : "");
                         if (i < 0 || i >= n) cc_error("runtime: string index out of range", 0);
                         PUSH(VI((unsigned char)s.s[i])); break; }
        case OP_BUILTIN: {
            int id = (int)in->a, argc = (int)in->b; Val *a = &st[sp - argc];
            Val r = VI(0);
            switch (id) {
            case B_LEN: r = VI(a[0].t == 1 ? (long)strlen(a[0].s ? a[0].s : "") : 0); break;
            case B_STR: { const char *cs = val_cstr(a[0]); int l=(int)strlen(cs); char*d=rt_alloc(l+1); memcpy(d,cs,l+1); r=(Val){1,0,d}; break; }
            case B_INT: { if (a[0].t == 1) { r = VI(atoi(a[0].s ? a[0].s : "0")); } else r = VI(a[0].i); break; }
            case B_ABS: { long v = need_int(a[0],"abs"); r = VI(v < 0 ? -v : v); break; }
            case B_MIN: { long m = need_int(a[0],"min"); for (int i=1;i<argc;i++){long v=need_int(a[i],"min"); if(v<m)m=v;} r=VI(m); break; }
            case B_MAX: { long m = need_int(a[0],"max"); for (int i=1;i<argc;i++){long v=need_int(a[i],"max"); if(v>m)m=v;} r=VI(m); break; }
            case B_CHR: { long v = need_int(a[0],"chr"); char*d=rt_alloc(2); d[0]=(char)v; d[1]=0; r=(Val){1,0,d}; break; }
            case B_ORD: { r = VI(a[0].t==1 ? (unsigned char)(a[0].s?a[0].s[0]:0) : a[0].i); break; }
            }
            sp -= argc; PUSH(r); break; }
        case OP_HALT: return;
        default: cc_error("internal: bad opcode", 0);
        }
        if (ip < 0) return;   /* returned from top frame */
    }
    #undef PUSH
    #undef POP
    #undef VI
}

/* ============================================================ driver ======= */
const char *boltcc_lang_name(int lang) {
    return lang == BCC_CPP ? "C++" : lang == BCC_CSHARP ? "C#" : "C";
}
const char *boltcc_version(void) { return "BoltCC 1.0  -  C / C++ / C# -> bytecode"; }

int boltcc_run(int lang, const char *src) {
    CC ctx; memset(&ctx, 0, sizeof(ctx));
    C = &ctx;
    C->lang = lang;
    C->src  = src;
    g_haderr = 0;

    int rc = 0;
    if (__builtin_setjmp(g_errjmp)) { rc = 1; goto cleanup; }

    C->strcap = 256 * 1024; C->strarena = (char *)kmalloc(C->strcap);
    C->astcap = 1024 * 1024; C->astarena = (char *)kmalloc(C->astcap);
    if (!C->strarena || !C->astarena) cc_error("out of memory", 0);

    lex();
    parse_program();

    int mi = func_find(lang == BCC_CSHARP ? "Main" : "main");
    if (mi < 0) cc_error(lang == BCC_CSHARP ? "no Main() entry point" : "no main() function", 0);

    /* codegen every function body */
    for (int i = 0; i < C->nfunc; i++) {
        Func *f = &C->funcs[i];
        C->nloc = 0; C->maxloc = 0; C->nloop = 0;
        for (int p = 0; p < f->nparams; p++) loc_add(f->params[p]);
        f->entry = C->ncode;
        gen_stmt(f->body);
        emit(OP_PUSHI, 0, 0, 0);     /* implicit  return 0;  on fall-through */
        emit(OP_RET, 0, 0, 0);
        if (C->maxloc > MAX_LOCALS) cc_error("function uses too many locals", 0);
    }

    /* preamble: run global initialisers, then call main/Main, then halt */
    int start = C->ncode;
    for (int i = 0; i < C->nglob; i++) {
        if (C->ginit[i]) {
            C->nloc = 0;               /* globals init runs with no locals */
            gen_expr(C->ginit[i]);
            emit(OP_STOREG, i, 0, 0);
        }
    }
    if (C->funcs[mi].nparams != 0) {
        /* Main(string[] args): pass an empty-arg placeholder (0) */
        for (int i = 0; i < C->funcs[mi].nparams; i++) emit(OP_PUSHI, 0, 0, 0);
        emit(OP_CALL, mi, C->funcs[mi].nparams, 0);
    } else {
        emit(OP_CALL, mi, 0, 0);
    }
    emit(OP_POP, 0, 0, 0);
    emit(OP_HALT, 0, 0, 0);

    /* runtime string scratch + global store reset */
    for (int i = 0; i < MAX_GLOBALS; i++) g_globstore[i] = (Val){0,0,0};
    g_rtcap = 1024 * 1024; g_rtpos = 0; g_rt = (char *)kmalloc(g_rtcap);
    if (!g_rt) cc_error("out of memory (runtime)", 0);

    vm_run(start);

cleanup:
    if (C->toks) kfree(C->toks);
    if (C->code) kfree(C->code);
    if (C->strarena) kfree(C->strarena);
    if (C->astarena) kfree(C->astarena);
    if (g_rt) { kfree(g_rt); g_rt = 0; }
    if (g_vm_st) { kfree(g_vm_st); g_vm_st = 0; }
    if (g_vm_frames) { kfree(g_vm_frames); g_vm_frames = 0; }
    C = 0;
    return rc;
}
