/* ===========================================================================
 *  BoltJS -- a tree-walking JavaScript interpreter for the BoltOS browser.
 *  Lexer -> recursive-descent parser (precedence climbing) -> AST walker.
 *
 *  Numbers are int64 (the kernel runs with -mno-sse/-mno-387, no soft-float --
 *  same convention as BoltPy), so `/` is integer division. Everything is
 *  allocated from a bump arena that is freed wholesale when js_run() returns;
 *  there is no garbage collector, which is fine for page-load-scoped scripts.
 * ===========================================================================*/
#include <stdint.h>
#include "js.h"
#include "string.h"
#include "kheap.h"

/* ----------------------------- arena ----------------------------------- */
#define ARENA_BLK (64 * 1024)
#define ARENA_MAX (8 * 1024 * 1024)     /* hard ceiling, runaway backstop */

typedef struct ABlk { struct ABlk *next; uint32_t used; uint8_t mem[ARENA_BLK]; } ABlk;

typedef struct {
    ABlk    *head;
    uint32_t total;
    int      oom;
} Arena;

static void *arena_alloc(Arena *a, uint32_t n) {
    n = (n + 7) & ~7u;                          /* 8-byte align */
    if (n > ARENA_BLK) { a->oom = 1; return 0; }
    if (!a->head || a->head->used + n > ARENA_BLK) {
        if (a->total + ARENA_BLK > ARENA_MAX) { a->oom = 1; return 0; }
        ABlk *b = (ABlk *)kmalloc(sizeof(ABlk));
        if (!b) { a->oom = 1; return 0; }
        b->next = a->head; b->used = 0; a->head = b; a->total += ARENA_BLK;
    }
    void *p = a->head->mem + a->head->used;
    a->head->used += n;
    return p;
}
static void arena_free(Arena *a) {
    for (ABlk *b = a->head; b; ) { ABlk *n = b->next; kfree(b); b = n; }
    a->head = 0; a->total = 0;
}
static char *arena_str(Arena *a, const char *s, uint32_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    if (!p) return 0;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ----------------------------- values ---------------------------------- */
typedef enum { T_UNDEF, T_NULL, T_BOOL, T_NUM, T_STR, T_OBJ } JType;
typedef struct JObj JObj;
typedef struct JVal { JType t; union { int64_t num; int b; char *str; JObj *obj; } u; } JVal;

typedef struct Node Node;
typedef struct JEnv JEnv;
typedef struct Interp Interp;

typedef struct Prop { char *key; JVal val; struct Prop *next; } Prop;

/* object kinds; HTAG marks a host singleton (console/Math/document/JSON) */
enum { K_OBJ, K_ARR, K_FUN, K_NATIVE };
enum { H_NONE, H_CONSOLE, H_MATH, H_DOCUMENT, H_JSON, H_WINDOW, H_LOCALSTORAGE,
       H_PROMISE, H_RESPONSE };

typedef JVal (*NativeFn)(Interp *, JVal thisv, JVal *args, int nargs);

struct JObj {
    int    kind;
    int    htag;
    Prop  *props;              /* OBJ properties                      */
    JVal  *items; int len, cap;/* ARR storage                          */
    Node  *params, *body;      /* FUN definition                       */
    JEnv  *closure;
    NativeFn nat;              /* NATIVE                               */
    JVal   bound_this;         /* NATIVE bound receiver (for methods)  */
    js_dom_node dom;           /* non-null: this object wraps a DOM node */
};

static JVal jundef(void){ JVal v; v.t=T_UNDEF; v.u.num=0; return v; }
static JVal jnull(void){ JVal v; v.t=T_NULL; v.u.num=0; return v; }
static JVal jbool(int b){ JVal v; v.t=T_BOOL; v.u.b=b?1:0; return v; }
static JVal jnum(int64_t n){ JVal v; v.t=T_NUM; v.u.num=n; return v; }
static JVal jstr(char *s){ JVal v; v.t=T_STR; v.u.str=s; return v; }
static JVal jobj(JObj *o){ JVal v; v.t=T_OBJ; v.u.obj=o; return v; }

/* ----------------------------- environment ----------------------------- */
struct JEnv { Prop *vars; JEnv *parent; };

/* ----------------------------- AST ------------------------------------- */
enum {
    N_NUM, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT, N_ARR, N_OBJ, N_FUN,
    N_MEMBER, N_INDEX, N_CALL, N_NEW, N_UNARY, N_UPDATE, N_BIN, N_LOGIC,
    N_ASSIGN, N_COND, N_SEQ,
    N_VAR, N_BLOCK, N_IF, N_WHILE, N_FOR, N_FORIN, N_RET, N_BREAK, N_CONT,
    N_EXPR, N_FUNDECL, N_EMPTY
};

struct Node {
    int   type;
    int64_t num;          /* N_NUM */
    char *str;            /* N_STR / N_IDENT / op spelling / member name */
    int   op;             /* operator token for BIN/UNARY/ASSIGN/UPDATE */
    int   prefix;         /* N_UPDATE prefix flag */
    Node *a, *b, *c, *d;  /* children */
    Node **list; int nlist;   /* arrays of children (args, params, elems, stmts) */
    char **keys;              /* object literal keys (paired with list values) */
};

/* ----------------------------- lexer ----------------------------------- */
enum {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT,
    TK_PUNCT
};

typedef struct { int k; int64_t num; char *str; const char *p; int op; } Tok;

/* operator/punctuator ids (also used as Node.op) */
enum {
    OP_NONE,
    OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH, OP_PCT,
    OP_ASSIGN, OP_PLUSEQ, OP_MINUSEQ, OP_STAREQ, OP_SLASHEQ, OP_PCTEQ,
    OP_EQ, OP_NEQ, OP_SEQ, OP_SNEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT, OP_INC, OP_DEC,
    OP_LPAREN, OP_RPAREN, OP_LBRACE, OP_RBRACE, OP_LBRACK, OP_RBRACK,
    OP_DOT, OP_COMMA, OP_SEMI, OP_COLON, OP_QUEST, OP_ARROW,
    OP_AMP, OP_PIPE, OP_CARET, OP_TILDE, OP_SHL, OP_SHR
};

struct Interp {
    Arena    a;
    const char *src; uint32_t slen, pos;
    Tok      cur, peeked; int has_peek;
    JEnv    *global;
    js_host *host;
    char     err[160]; int errflag;
    int      flow;            /* 0 normal, 1 return, 2 break, 3 continue */
    JVal     retval;
    int      steps;           /* execution budget guard */
    uint64_t now_ms;          /* current time, set by js_vm_pump (for timers) */
    /* persistent-VM registries (timers + event listeners) */
    struct { JObj *fn; uint64_t due; uint32_t interval; int active; } timers[32];
    int      ntimers;
    struct { js_dom_node target; char type[20]; JObj *fn; int active; } listeners[64];
    int      nlisteners;
    /* promise microtask FIFO (drained after eval/pump/dispatch) */
    struct { JObj *onF, *onR, *rp; int state; JVal val; } microq[128];
    int      mq_head, mq_count;
};

static void seterr(Interp *I, const char *m) {
    if (I->errflag) return;
    I->errflag = 1;
    uint32_t i = 0; for (; m[i] && i < sizeof(I->err)-1; i++) I->err[i] = m[i]; I->err[i] = 0;
}

static int is_id_start(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static int is_id_part(int c){ return is_id_start(c)||(c>='0'&&c<='9'); }
static int is_digit(int c){ return c>='0'&&c<='9'; }

/* read one token from src at I->pos */
static Tok lex(Interp *I) {
    Tok t; t.k = TK_EOF; t.str = 0; t.num = 0; t.op = OP_NONE;
    const char *s = I->src; uint32_t n = I->slen;
    /* skip whitespace + comments */
    for (;;) {
        while (I->pos < n && (s[I->pos]==' '||s[I->pos]=='\t'||s[I->pos]=='\r'||s[I->pos]=='\n')) I->pos++;
        if (I->pos+1 < n && s[I->pos]=='/' && s[I->pos+1]=='/') {
            I->pos += 2; while (I->pos < n && s[I->pos] != '\n') I->pos++; continue;
        }
        if (I->pos+1 < n && s[I->pos]=='/' && s[I->pos+1]=='*') {
            I->pos += 2; while (I->pos+1 < n && !(s[I->pos]=='*'&&s[I->pos+1]=='/')) I->pos++;
            if (I->pos+1 < n) I->pos += 2; continue;
        }
        break;
    }
    if (I->pos >= n) return t;
    int c = s[I->pos];

    if (is_digit(c) || (c=='.' && I->pos+1<n && is_digit(s[I->pos+1]))) {
        int64_t v = 0;
        if (c=='0' && I->pos+1<n && (s[I->pos+1]=='x'||s[I->pos+1]=='X')) {
            I->pos += 2;
            while (I->pos<n) { int d=s[I->pos];
                int hv = (d>='0'&&d<='9')?d-'0':(d>='a'&&d<='f')?d-'a'+10:(d>='A'&&d<='F')?d-'A'+10:-1;
                if (hv<0) break; v = v*16+hv; I->pos++; }
        } else {
            while (I->pos<n && is_digit(s[I->pos])) { v = v*10 + (s[I->pos]-'0'); I->pos++; }
            if (I->pos<n && s[I->pos]=='.') { I->pos++; while (I->pos<n && is_digit(s[I->pos])) I->pos++; } /* drop fraction */
            if (I->pos<n && (s[I->pos]=='e'||s[I->pos]=='E')) { I->pos++; if(I->pos<n&&(s[I->pos]=='+'||s[I->pos]=='-'))I->pos++; while(I->pos<n&&is_digit(s[I->pos]))I->pos++; }
        }
        t.k = TK_NUM; t.num = v; return t;
    }
    if (is_id_start(c)) {
        uint32_t st = I->pos;
        while (I->pos<n && is_id_part(s[I->pos])) I->pos++;
        t.k = TK_IDENT; t.str = arena_str(&I->a, s+st, I->pos-st); return t;
    }
    if (c=='"' || c=='\'' || c=='`') {
        int q = c; I->pos++;
        char buf[1024]; uint32_t bl = 0;
        while (I->pos<n && s[I->pos]!=q) {
            int ch = s[I->pos++];
            if (ch=='\\' && I->pos<n) {
                int e = s[I->pos++];
                ch = (e=='n')?'\n':(e=='t')?'\t':(e=='r')?'\r':(e=='\\')?'\\':
                     (e=='\'')?'\'':(e=='"')?'"':(e=='`')?'`':(e=='0')?0:(e=='b')?'\b':e;
                if (e=='u') { /* \uXXXX -> '?' (ASCII fonts) */ for(int k=0;k<4&&I->pos<n;k++)I->pos++; ch='?'; }
            }
            if (bl < sizeof(buf)-1) buf[bl++] = (char)ch;
        }
        if (I->pos<n) I->pos++;             /* closing quote */
        t.k = TK_STR; t.str = arena_str(&I->a, buf, bl); return t;
    }

    /* punctuators / operators */
    t.k = TK_PUNCT;
    #define TWO(x,y,o) if(c==x && I->pos+1<n && s[I->pos+1]==y){ I->pos+=2; t.op=o; return t; }
    #define THREE(x,y,z,o) if(c==x && I->pos+2<n && s[I->pos+1]==y && s[I->pos+2]==z){ I->pos+=3; t.op=o; return t; }
    THREE('=','=','=',OP_SEQ) THREE('!','=','=',OP_SNEQ)
    TWO('=','=',OP_EQ) TWO('!','=',OP_NEQ) TWO('<','=',OP_LE) TWO('>','=',OP_GE)
    TWO('&','&',OP_AND) TWO('|','|',OP_OR) TWO('+','+',OP_INC) TWO('-','-',OP_DEC)
    TWO('+','=',OP_PLUSEQ) TWO('-','=',OP_MINUSEQ) TWO('*','=',OP_STAREQ)
    TWO('/','=',OP_SLASHEQ) TWO('%','=',OP_PCTEQ)
    TWO('=','>',OP_ARROW) TWO('<','<',OP_SHL) TWO('>','>',OP_SHR)
    #undef TWO
    #undef THREE
    I->pos++;
    switch (c) {
        case '+': t.op=OP_PLUS; break;  case '-': t.op=OP_MINUS; break;
        case '*': t.op=OP_STAR; break;  case '/': t.op=OP_SLASH; break;
        case '%': t.op=OP_PCT; break;   case '=': t.op=OP_ASSIGN; break;
        case '<': t.op=OP_LT; break;    case '>': t.op=OP_GT; break;
        case '!': t.op=OP_NOT; break;   case '(': t.op=OP_LPAREN; break;
        case ')': t.op=OP_RPAREN; break;case '{': t.op=OP_LBRACE; break;
        case '}': t.op=OP_RBRACE; break;case '[': t.op=OP_LBRACK; break;
        case ']': t.op=OP_RBRACK; break;case '.': t.op=OP_DOT; break;
        case ',': t.op=OP_COMMA; break; case ';': t.op=OP_SEMI; break;
        case ':': t.op=OP_COLON; break; case '?': t.op=OP_QUEST; break;
        case '&': t.op=OP_AMP; break;   case '|': t.op=OP_PIPE; break;
        case '^': t.op=OP_CARET; break; case '~': t.op=OP_TILDE; break;
        default: t.op=OP_NONE; break;
    }
    return t;
}

static Tok *peek(Interp *I) {
    if (!I->has_peek) { I->peeked = lex(I); I->has_peek = 1; }
    return &I->peeked;
}
static Tok next(Interp *I) {
    if (I->has_peek) { I->has_peek = 0; I->cur = I->peeked; return I->cur; }
    I->cur = lex(I); return I->cur;
}
static int is_op(Tok *t, int op){ return t->k==TK_PUNCT && t->op==op; }
static int is_kw(Tok *t, const char *kw){ return t->k==TK_IDENT && strcmp(t->str,kw)==0; }
static int eat_op(Interp *I, int op){ if(is_op(peek(I),op)){ next(I); return 1; } return 0; }
static int eat_kw(Interp *I, const char *kw){ if(is_kw(peek(I),kw)){ next(I); return 1; } return 0; }

/* ----------------------------- parser ---------------------------------- */
static Node *new_node(Interp *I, int type) {
    Node *n = (Node *)arena_alloc(&I->a, sizeof(Node));
    if (!n) { seterr(I, "out of memory"); return 0; }
    memset(n, 0, sizeof(*n)); n->type = type; return n;
}
static Node *parse_expr(Interp *I);
static Node *parse_assign(Interp *I);
static Node *parse_stmt(Interp *I);

static Node **node_list(Interp *I, Node **tmp, int cnt) {
    Node **arr = (Node **)arena_alloc(&I->a, sizeof(Node*) * (cnt?cnt:1));
    for (int i=0;i<cnt;i++) arr[i]=tmp[i];
    return arr;
}

static Node *parse_primary(Interp *I) {
    Tok *t = peek(I);
    if (t->k == TK_NUM)  { next(I); Node *n=new_node(I,N_NUM); if(n)n->num=I->cur.num; return n; }
    if (t->k == TK_STR)  { next(I); Node *n=new_node(I,N_STR); if(n)n->str=I->cur.str; return n; }
    if (t->k == TK_IDENT) {
        if (is_kw(t,"true")) { next(I); Node *n=new_node(I,N_BOOL); if(n)n->num=1; return n; }
        if (is_kw(t,"false")){ next(I); Node *n=new_node(I,N_BOOL); if(n)n->num=0; return n; }
        if (is_kw(t,"null")) { next(I); return new_node(I,N_NULL); }
        if (is_kw(t,"undefined")){ next(I); return new_node(I,N_UNDEF); }
        if (is_kw(t,"function")) {
            next(I);
            Node *fn = new_node(I,N_FUN); if(!fn) return 0;
            if (peek(I)->k==TK_IDENT) { next(I); fn->str = I->cur.str; }  /* name (optional) */
            if (!eat_op(I,OP_LPAREN)) { seterr(I,"expected ("); return 0; }
            Node *ps[32]; int pc=0;
            while (!is_op(peek(I),OP_RPAREN)) {
                if (peek(I)->k!=TK_IDENT){ seterr(I,"bad param"); return 0; }
                next(I); Node *p=new_node(I,N_IDENT); if(p)p->str=I->cur.str;
                if(pc<32)ps[pc++]=p;
                if (!eat_op(I,OP_COMMA)) break;
            }
            if (!eat_op(I,OP_RPAREN)) { seterr(I,"expected )"); return 0; }
            fn->list = node_list(I,ps,pc); fn->nlist = pc;
            fn->a = parse_stmt(I);   /* body block */
            return fn;
        }
        next(I); Node *n=new_node(I,N_IDENT); if(n)n->str=t->str; return n;
    }
    if (is_op(t,OP_LPAREN)) {
        next(I);
        Node *e = parse_expr(I);
        if (!eat_op(I,OP_RPAREN)) { seterr(I,"expected )"); }
        return e;
    }
    if (is_op(t,OP_LBRACK)) {            /* array literal */
        next(I);
        Node *n = new_node(I,N_ARR); if(!n) return 0;
        Node *el[64]; int c=0;
        while (!is_op(peek(I),OP_RBRACK)) {
            Node *e = parse_assign(I); if(c<64)el[c++]=e;
            if (!eat_op(I,OP_COMMA)) break;
        }
        eat_op(I,OP_RBRACK);
        n->list = node_list(I,el,c); n->nlist=c; return n;
    }
    if (is_op(t,OP_LBRACE)) {            /* object literal */
        next(I);
        Node *n = new_node(I,N_OBJ); if(!n) return 0;
        Node *vals[64]; char *keys[64]; int c=0;
        while (!is_op(peek(I),OP_RBRACE)) {
            char *key=0;
            Tok *k = peek(I);
            if (k->k==TK_IDENT) { next(I); key=I->cur.str; }
            else if (k->k==TK_STR){ next(I); key=I->cur.str; }
            else if (k->k==TK_NUM){ next(I); char b[24]; char *p=b; int64_t v=I->cur.num; /* simple itoa */
                                    char tmp[24]; int ti=0; if(v==0)tmp[ti++]='0'; int neg=v<0; uint64_t uu=neg?-(uint64_t)v:(uint64_t)v; while(uu){tmp[ti++]='0'+uu%10;uu/=10;} if(neg)tmp[ti++]='-'; for(int z=0;z<ti;z++)p[z]=tmp[ti-1-z]; p[ti]=0; key=arena_str(&I->a,b,ti); }
            else break;
            if (!eat_op(I,OP_COLON)) { seterr(I,"expected :"); return 0; }
            Node *v = parse_assign(I);
            if (c<64){ keys[c]=key; vals[c]=v; c++; }
            if (!eat_op(I,OP_COMMA)) break;
        }
        eat_op(I,OP_RBRACE);
        n->list = node_list(I,vals,c); n->nlist=c;
        n->keys = (char**)arena_alloc(&I->a, sizeof(char*)*(c?c:1));
        for(int i=0;i<c;i++) n->keys[i]=keys[i];
        return n;
    }
    if (is_kw(t,"new")) {
        next(I);
        Node *callee = parse_primary(I);
        Node *n = new_node(I,N_NEW); if(!n) return 0;
        n->a = callee;
        Node *args[32]; int ac=0;
        if (eat_op(I,OP_LPAREN)) {
            while (!is_op(peek(I),OP_RPAREN)) { Node*e=parse_assign(I); if(ac<32)args[ac++]=e; if(!eat_op(I,OP_COMMA))break; }
            eat_op(I,OP_RPAREN);
        }
        n->list=node_list(I,args,ac); n->nlist=ac;
        return n;
    }
    seterr(I,"unexpected token");
    next(I);
    return new_node(I,N_UNDEF);
}

/* member access, indexing, calls (left-assoc postfix) */
static Node *parse_postfix(Interp *I) {
    Node *e = parse_primary(I);
    for (;;) {
        if (eat_op(I,OP_DOT)) {
            if (peek(I)->k!=TK_IDENT){ seterr(I,"expected name after ."); break; }
            next(I);
            Node *m=new_node(I,N_MEMBER); if(!m)break; m->a=e; m->str=I->cur.str; e=m;
        } else if (eat_op(I,OP_LBRACK)) {
            Node *idx=parse_expr(I);
            eat_op(I,OP_RBRACK);
            Node *m=new_node(I,N_INDEX); if(!m)break; m->a=e; m->b=idx; e=m;
        } else if (is_op(peek(I),OP_LPAREN)) {
            next(I);
            Node *args[32]; int ac=0;
            while (!is_op(peek(I),OP_RPAREN)) { Node*a=parse_assign(I); if(ac<32)args[ac++]=a; if(!eat_op(I,OP_COMMA))break; }
            eat_op(I,OP_RPAREN);
            Node *c=new_node(I,N_CALL); if(!c)break; c->a=e; c->list=node_list(I,args,ac); c->nlist=ac; e=c;
        } else if (is_op(peek(I),OP_INC) || is_op(peek(I),OP_DEC)) {
            int op = peek(I)->op; next(I);
            Node *u=new_node(I,N_UPDATE); if(!u)break; u->a=e; u->op=op; u->prefix=0; e=u;
        } else break;
    }
    return e;
}

static Node *parse_unary(Interp *I) {
    Tok *t = peek(I);
    if (is_op(t,OP_NOT)||is_op(t,OP_MINUS)||is_op(t,OP_PLUS)||is_op(t,OP_TILDE)) {
        int op=t->op; next(I);
        Node *n=new_node(I,N_UNARY); if(!n)return 0; n->op=op; n->a=parse_unary(I); return n;
    }
    if (is_kw(t,"typeof")) { next(I); Node*n=new_node(I,N_UNARY); if(!n)return 0; n->op=OP_NONE; n->str="typeof"; n->a=parse_unary(I); return n; }
    if (is_op(t,OP_INC)||is_op(t,OP_DEC)) {
        int op=t->op; next(I);
        Node *n=new_node(I,N_UPDATE); if(!n)return 0; n->op=op; n->prefix=1; n->a=parse_unary(I); return n;
    }
    return parse_postfix(I);
}

/* binary precedence */
static int bin_prec(int op) {
    switch(op){
        case OP_STAR: case OP_SLASH: case OP_PCT: return 11;
        case OP_PLUS: case OP_MINUS: return 10;
        case OP_SHL: case OP_SHR: return 9;
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 8;
        case OP_EQ: case OP_NEQ: case OP_SEQ: case OP_SNEQ: return 7;
        case OP_AMP: return 6; case OP_CARET: return 5; case OP_PIPE: return 4;
        case OP_AND: return 3; case OP_OR: return 2;
        default: return -1;
    }
}
static Node *parse_bin(Interp *I, int minp) {
    Node *left = parse_unary(I);
    for (;;) {
        Tok *t = peek(I);
        if (t->k != TK_PUNCT) break;
        int p = bin_prec(t->op);
        if (p < minp || p < 0) break;
        int op = t->op; next(I);
        Node *right = parse_bin(I, p+1);
        int isl = (op==OP_AND||op==OP_OR);
        Node *n=new_node(I, isl?N_LOGIC:N_BIN); if(!n)break;
        n->op=op; n->a=left; n->b=right; left=n;
    }
    return left;
}

static Node *parse_cond(Interp *I) {
    Node *c = parse_bin(I, 1);
    if (eat_op(I,OP_QUEST)) {
        Node *n=new_node(I,N_COND); if(!n)return c;
        n->a=c; n->b=parse_assign(I);
        eat_op(I,OP_COLON);
        n->c=parse_assign(I);
        return n;
    }
    return c;
}

static int is_assign_op(int op){
    return op==OP_ASSIGN||op==OP_PLUSEQ||op==OP_MINUSEQ||op==OP_STAREQ||op==OP_SLASHEQ||op==OP_PCTEQ;
}
static Node *parse_assign(Interp *I) {
    Node *l = parse_cond(I);
    Tok *t = peek(I);
    if (t->k==TK_PUNCT && is_assign_op(t->op)) {
        int op=t->op; next(I);
        Node *n=new_node(I,N_ASSIGN); if(!n)return l; n->op=op; n->a=l; n->b=parse_assign(I); return n;
    }
    return l;
}
static Node *parse_expr(Interp *I) {
    Node *e = parse_assign(I);
    while (is_op(peek(I),OP_COMMA)) { next(I); Node*n=new_node(I,N_SEQ); if(!n)break; n->a=e; n->b=parse_assign(I); e=n; }
    return e;
}

static Node *parse_block(Interp *I) {
    Node *n = new_node(I,N_BLOCK); if(!n) return 0;
    Node *st[256]; int c=0;
    eat_op(I,OP_LBRACE);
    while (!is_op(peek(I),OP_RBRACE) && peek(I)->k!=TK_EOF) {
        Node *s = parse_stmt(I); if(c<256 && s)st[c++]=s;
        if (I->errflag) break;
    }
    eat_op(I,OP_RBRACE);
    n->list=node_list(I,st,c); n->nlist=c; return n;
}

static Node *parse_var(Interp *I) {
    next(I); /* var/let/const */
    Node *n=new_node(I,N_VAR); if(!n) return 0;
    Node *names[16]; Node *inits[16]; int c=0;
    for (;;) {
        if (peek(I)->k!=TK_IDENT){ seterr(I,"expected name"); break; }
        next(I); Node *nm=new_node(I,N_IDENT); if(nm)nm->str=I->cur.str;
        Node *init=0;
        if (eat_op(I,OP_ASSIGN)) init=parse_assign(I);
        if (c<16){ names[c]=nm; inits[c]=init; c++; }
        if (!eat_op(I,OP_COMMA)) break;
    }
    n->list=node_list(I,names,c); n->nlist=c;
    n->keys=0;
    /* stash inits in a parallel Node** via ->d list trick: reuse 'list' for names, store inits in arena */
    Node **iv = (Node**)arena_alloc(&I->a, sizeof(Node*)*(c?c:1));
    for(int i=0;i<c;i++) iv[i]=inits[i];
    n->b = (Node*)0; n->c = (Node*)iv;  /* c holds inits array (cast) */
    eat_op(I,OP_SEMI);
    return n;
}

static Node *parse_stmt(Interp *I) {
    Tok *t = peek(I);
    if (is_op(t,OP_LBRACE)) return parse_block(I);
    if (is_op(t,OP_SEMI)) { next(I); return new_node(I,N_EMPTY); }
    if (t->k==TK_IDENT) {
        if (is_kw(t,"var")||is_kw(t,"let")||is_kw(t,"const")) return parse_var(I);
        if (is_kw(t,"function")) {
            next(I);
            Node *fn=new_node(I,N_FUNDECL); if(!fn)return 0;
            if (peek(I)->k==TK_IDENT){ next(I); fn->str=I->cur.str; }
            eat_op(I,OP_LPAREN);
            Node *ps[32]; int pc=0;
            while (!is_op(peek(I),OP_RPAREN)) { if(peek(I)->k!=TK_IDENT){seterr(I,"bad param");break;} next(I); Node*p=new_node(I,N_IDENT); if(p)p->str=I->cur.str; if(pc<32)ps[pc++]=p; if(!eat_op(I,OP_COMMA))break; }
            eat_op(I,OP_RPAREN);
            fn->list=node_list(I,ps,pc); fn->nlist=pc;
            fn->a=parse_block(I);
            return fn;
        }
        if (is_kw(t,"if")) {
            next(I); eat_op(I,OP_LPAREN); Node*c=parse_expr(I); eat_op(I,OP_RPAREN);
            Node*n=new_node(I,N_IF); if(!n)return 0; n->a=c; n->b=parse_stmt(I);
            if (eat_kw(I,"else")) n->c=parse_stmt(I);
            return n;
        }
        if (is_kw(t,"while")) {
            next(I); eat_op(I,OP_LPAREN); Node*c=parse_expr(I); eat_op(I,OP_RPAREN);
            Node*n=new_node(I,N_WHILE); if(!n)return 0; n->a=c; n->b=parse_stmt(I); return n;
        }
        if (is_kw(t,"for")) {
            next(I); eat_op(I,OP_LPAREN);
            /* detect for-in: (var? IDENT in EXPR) */
            Node *init=0;
            if (is_kw(peek(I),"var")||is_kw(peek(I),"let")||is_kw(peek(I),"const")) {
                /* could be for-in or classic */
                int save=I->pos; Tok savep=I->peeked; int savehp=I->has_peek;
                next(I); /* var */
                if (peek(I)->k==TK_IDENT) {
                    next(I); char *nm=I->cur.str;
                    if (is_kw(peek(I),"in")) {
                        next(I); Node*obj=parse_expr(I); eat_op(I,OP_RPAREN);
                        Node*n=new_node(I,N_FORIN); if(!n)return 0;
                        Node*idn=new_node(I,N_IDENT); if(idn)idn->str=nm;
                        n->a=idn; n->b=obj; n->c=parse_stmt(I); return n;
                    }
                }
                /* not for-in: rewind */
                I->pos=save; I->peeked=savep; I->has_peek=savehp;
                init = parse_var(I);   /* consumes its own ; */
            } else if (!is_op(peek(I),OP_SEMI)) {
                init=new_node(I,N_EXPR); if(init)init->a=parse_expr(I); eat_op(I,OP_SEMI);
            } else eat_op(I,OP_SEMI);
            Node *cond=0; if(!is_op(peek(I),OP_SEMI)) cond=parse_expr(I); eat_op(I,OP_SEMI);
            Node *upd=0;  if(!is_op(peek(I),OP_RPAREN)) upd=parse_expr(I); eat_op(I,OP_RPAREN);
            Node *n=new_node(I,N_FOR); if(!n)return 0; n->a=init; n->b=cond; n->c=upd; n->d=parse_stmt(I); return n;
        }
        if (is_kw(t,"return")) {
            next(I); Node*n=new_node(I,N_RET); if(!n)return 0;
            if (!is_op(peek(I),OP_SEMI) && !is_op(peek(I),OP_RBRACE) && peek(I)->k!=TK_EOF) n->a=parse_expr(I);
            eat_op(I,OP_SEMI); return n;
        }
        if (is_kw(t,"break")) { next(I); eat_op(I,OP_SEMI); return new_node(I,N_BREAK); }
        if (is_kw(t,"continue")) { next(I); eat_op(I,OP_SEMI); return new_node(I,N_CONT); }
    }
    Node *n=new_node(I,N_EXPR); if(!n)return 0; n->a=parse_expr(I); eat_op(I,OP_SEMI); return n;
}

/* ----------------------------- interpreter ----------------------------- */
static JVal eval(Interp *I, Node *n, JEnv *env);
static JVal call_fn(Interp *I, JObj *fn, JVal thisv, JVal *args, int nargs);
static char *val_to_str(Interp *I, JVal v);
static int   truthy(JVal v);

static JObj *new_obj(Interp *I, int kind) {
    JObj *o=(JObj*)arena_alloc(&I->a,sizeof(JObj)); if(!o){seterr(I,"oom");return 0;}
    memset(o,0,sizeof(*o)); o->kind=kind; return o;
}
static void obj_set(Interp *I, JObj *o, const char *key, JVal v) {
    for (Prop *p=o->props;p;p=p->next) if(strcmp(p->key,key)==0){ p->val=v; return; }
    Prop *p=(Prop*)arena_alloc(&I->a,sizeof(Prop)); if(!p)return;
    p->key=arena_str(&I->a,key,strlen(key)); p->val=v; p->next=o->props; o->props=p;
}
static int obj_get(JObj *o, const char *key, JVal *out) {
    for (Prop *p=o->props;p;p=p->next) if(strcmp(p->key,key)==0){ *out=p->val; return 1; }
    return 0;
}
static void arr_push(Interp *I, JObj *a, JVal v) {
    if (a->len>=a->cap) {
        int nc = a->cap? a->cap*2 : 8;
        JVal *ni=(JVal*)arena_alloc(&I->a,sizeof(JVal)*nc); if(!ni)return;
        for(int i=0;i<a->len;i++) ni[i]=a->items[i];
        a->items=ni; a->cap=nc;
    }
    a->items[a->len++]=v;
}

/* environment ops */
static JEnv *new_env(Interp *I, JEnv *parent) {
    JEnv *e=(JEnv*)arena_alloc(&I->a,sizeof(JEnv)); if(!e)return parent;
    e->vars=0; e->parent=parent; return e;
}
static void env_def(Interp *I, JEnv *e, const char *name, JVal v) {
    for (Prop *p=e->vars;p;p=p->next) if(strcmp(p->key,name)==0){ p->val=v; return; }
    Prop *p=(Prop*)arena_alloc(&I->a,sizeof(Prop)); if(!p)return;
    p->key=arena_str(&I->a,name,strlen(name)); p->val=v; p->next=e->vars; e->vars=p;
}
static Prop *env_find(JEnv *e, const char *name) {
    for (; e; e=e->parent) for (Prop *p=e->vars;p;p=p->next) if(strcmp(p->key,name)==0) return p;
    return 0;
}

/* number/string formatting */
static char *i64_to_str(Interp *I, int64_t v) {
    char tmp[24]; int ti=0; int neg=v<0; uint64_t u=neg?(uint64_t)(-(v+1))+1u:(uint64_t)v;
    if(u==0)tmp[ti++]='0'; while(u){tmp[ti++]='0'+(int)(u%10);u/=10;} if(neg)tmp[ti++]='-';
    char out[24]; for(int i=0;i<ti;i++)out[i]=tmp[ti-1-i]; out[ti]=0;
    return arena_str(&I->a,out,ti);
}
static char *str_concat(Interp *I, const char *a, const char *b) {
    uint32_t la=strlen(a), lb=strlen(b);
    char *r=(char*)arena_alloc(&I->a,la+lb+1); if(!r)return "";
    memcpy(r,a,la); memcpy(r+la,b,lb); r[la+lb]=0; return r;
}

static const char *type_name(JVal v) {
    switch(v.t){
        case T_UNDEF: return "undefined"; case T_NULL: return "object";
        case T_BOOL: return "boolean"; case T_NUM: return "number";
        case T_STR: return "string";
        case T_OBJ: return v.u.obj && (v.u.obj->kind==K_FUN||v.u.obj->kind==K_NATIVE) ? "function" : "object";
    }
    return "undefined";
}

static int64_t to_num(JVal v) {
    switch(v.t){
        case T_NUM: return v.u.num; case T_BOOL: return v.u.b;
        case T_STR: { int64_t r=0; int neg=0; const char*s=v.u.str; while(*s==' ')s++; if(*s=='-'){neg=1;s++;} else if(*s=='+')s++; int any=0; while(*s>='0'&&*s<='9'){r=r*10+(*s-'0');s++;any=1;} return any?(neg?-r:r):0; }
        default: return 0;
    }
}
static int truthy(JVal v) {
    switch(v.t){
        case T_UNDEF: case T_NULL: return 0;
        case T_BOOL: return v.u.b; case T_NUM: return v.u.num!=0;
        case T_STR: return v.u.str[0]!=0;
        case T_OBJ: return 1;
    }
    return 0;
}
static char *val_to_str(Interp *I, JVal v) {
    switch(v.t){
        case T_UNDEF: return "undefined"; case T_NULL: return "null";
        case T_BOOL: return v.u.b? "true":"false";
        case T_NUM: return i64_to_str(I,v.u.num);
        case T_STR: return v.u.str;
        case T_OBJ: {
            JObj *o=v.u.obj;
            if (o->kind==K_FUN||o->kind==K_NATIVE) return "function";
            if (o->kind==K_ARR) {
                char *acc=""; for(int i=0;i<o->len;i++){ if(i)acc=str_concat(I,acc,","); acc=str_concat(I,acc,val_to_str(I,o->items[i])); } return acc;
            }
            return "[object Object]";
        }
    }
    return "undefined";
}

static int loose_eq(Interp *I, JVal a, JVal b) {
    if (a.t==b.t) {
        switch(a.t){
            case T_UNDEF: case T_NULL: return 1;
            case T_BOOL: return a.u.b==b.u.b;
            case T_NUM: return a.u.num==b.u.num;
            case T_STR: return strcmp(a.u.str,b.u.str)==0;
            case T_OBJ: return a.u.obj==b.u.obj;
        }
    }
    if ((a.t==T_NULL&&b.t==T_UNDEF)||(a.t==T_UNDEF&&b.t==T_NULL)) return 1;
    if (a.t==T_NUM||b.t==T_NUM||a.t==T_BOOL||b.t==T_BOOL) return to_num(a)==to_num(b);
    return 0;
}
static int strict_eq(JVal a, JVal b) {
    if (a.t!=b.t) return 0;
    switch(a.t){
        case T_UNDEF: case T_NULL: return 1;
        case T_BOOL: return a.u.b==b.u.b;
        case T_NUM: return a.u.num==b.u.num;
        case T_STR: return strcmp(a.u.str,b.u.str)==0;
        case T_OBJ: return a.u.obj==b.u.obj;
    }
    return 0;
}

/* forward: builtin member dispatch */
static int member_get(Interp *I, JVal recv, const char *name, JVal *out);
static JVal method_call(Interp *I, JVal recv, const char *name, JVal *args, int nargs, int *handled);

/* assignment target resolution */
static void assign_to(Interp *I, Node *target, JVal v, JEnv *env) {
    if (target->type==N_IDENT) {
        Prop *p=env_find(env,target->str);
        if (p) p->val=v; else env_def(I,I->global,target->str,v);
        return;
    }
    if (target->type==N_MEMBER) {
        JVal obj=eval(I,target->a,env);
        if (obj.t==T_OBJ) {
            JObj *o=obj.u.obj;
            if (o->dom && I->host) {
                if (strcmp(target->str,"innerHTML")==0||strcmp(target->str,"innerText")==0||strcmp(target->str,"textContent")==0) {
                    if (strcmp(target->str,"innerHTML")==0) I->host->set_inner(I->host->ud,o->dom,val_to_str(I,v));
                    else I->host->set_text(I->host->ud,o->dom,val_to_str(I,v));
                    return;
                }
                if (I->host->set_attr) {                  /* el.id / className / href / src / value */
                    if (strcmp(target->str,"id")==0)        { I->host->set_attr(I->host->ud,o->dom,"id",val_to_str(I,v)); return; }
                    if (strcmp(target->str,"className")==0)  { I->host->set_attr(I->host->ud,o->dom,"class",val_to_str(I,v)); return; }
                    if (strcmp(target->str,"href")==0||strcmp(target->str,"src")==0||strcmp(target->str,"value")==0)
                        { I->host->set_attr(I->host->ud,o->dom,target->str,val_to_str(I,v)); return; }
                }
            }
            if (o->htag==H_DOCUMENT && strcmp(target->str,"title")==0) { if(I->host)I->host->set_title(I->host->ud,val_to_str(I,v)); return; }
            if (o->htag==H_DOCUMENT && strcmp(target->str,"cookie")==0) { if(I->host&&I->host->cookie_set)I->host->cookie_set(I->host->ud,val_to_str(I,v)); return; }
            obj_set(I,o,target->str,v);
        }
        return;
    }
    if (target->type==N_INDEX) {
        JVal obj=eval(I,target->a,env); JVal idx=eval(I,target->b,env);
        if (obj.t==T_OBJ) {
            JObj *o=obj.u.obj;
            if (o->kind==K_ARR && idx.t==T_NUM) {
                int i=(int)idx.u.num;
                if (i>=0) { while(o->len<=i) arr_push(I,o,jundef()); o->items[i]=v; }
                return;
            }
            obj_set(I,o,val_to_str(I,idx),v);
        }
        return;
    }
    seterr(I,"invalid assignment target");
}

static JVal eval(Interp *I, Node *n, JEnv *env) {
    if (!n || I->errflag) return jundef();
    if (++I->steps > 50000000) { seterr(I,"script ran too long"); return jundef(); }

    switch (n->type) {
    case N_NUM:  return jnum(n->num);
    case N_STR:  return jstr(n->str);
    case N_BOOL: return jbool((int)n->num);
    case N_NULL: return jnull();
    case N_UNDEF:return jundef();
    case N_IDENT: {
        Prop *p=env_find(env,n->str);
        if (p) return p->val;
        return jundef();
    }
    case N_ARR: {
        JObj *o=new_obj(I,K_ARR); if(!o)return jundef();
        for(int i=0;i<n->nlist;i++) arr_push(I,o,eval(I,n->list[i],env));
        return jobj(o);
    }
    case N_OBJ: {
        JObj *o=new_obj(I,K_OBJ); if(!o)return jundef();
        for(int i=0;i<n->nlist;i++) obj_set(I,o,n->keys[i],eval(I,n->list[i],env));
        return jobj(o);
    }
    case N_FUN: {
        JObj *o=new_obj(I,K_FUN); if(!o)return jundef();
        o->params=(Node*)n; o->body=n->a; o->closure=env; return jobj(o);
    }
    case N_SEQ: { eval(I,n->a,env); return eval(I,n->b,env); }
    case N_COND: return truthy(eval(I,n->a,env)) ? eval(I,n->b,env) : eval(I,n->c,env);
    case N_MEMBER: {
        JVal recv=eval(I,n->a,env); JVal out;
        if (member_get(I,recv,n->str,&out)) return out;
        return jundef();
    }
    case N_INDEX: {
        JVal recv=eval(I,n->a,env); JVal idx=eval(I,n->b,env);
        if (recv.t==T_OBJ) {
            JObj *o=recv.u.obj;
            if (o->kind==K_ARR && idx.t==T_NUM) { int i=(int)idx.u.num; if(i>=0&&i<o->len) return o->items[i]; return jundef(); }
            JVal out; if(obj_get(o,val_to_str(I,idx),&out)) return out;
            if (member_get(I,recv,val_to_str(I,idx),&out)) return out;
            return jundef();
        }
        if (recv.t==T_STR) { int i=(int)to_num(idx); int L=strlen(recv.u.str); if(i>=0&&i<L){ char c[2]={recv.u.str[i],0}; return jstr(arena_str(&I->a,c,1)); } return jundef(); }
        return jundef();
    }
    case N_CALL: {
        /* method call vs plain call */
        Node *callee=n->a;
        JVal args[32]; int ac=n->nlist>32?32:n->nlist;
        for(int i=0;i<ac;i++) args[i]=eval(I,n->list[i],env);
        if (callee->type==N_MEMBER || callee->type==N_INDEX) {
            JVal recv=eval(I,callee->a,env);
            const char *mname = callee->type==N_MEMBER ? callee->str : val_to_str(I,eval(I,callee->b,env));
            int handled=0;
            JVal r=method_call(I,recv,mname,args,ac,&handled);
            if (handled) return r;
            /* fall back: property is a user function */
            JVal fn;
            if (recv.t==T_OBJ && obj_get(recv.u.obj,mname,&fn) && fn.t==T_OBJ && (fn.u.obj->kind==K_FUN||fn.u.obj->kind==K_NATIVE))
                return call_fn(I,fn.u.obj,recv,args,ac);
            seterr(I,"not a function"); return jundef();
        }
        JVal fn=eval(I,callee,env);
        if (fn.t==T_OBJ && (fn.u.obj->kind==K_FUN||fn.u.obj->kind==K_NATIVE)) return call_fn(I,fn.u.obj,jundef(),args,ac);
        seterr(I,"not a function"); return jundef();
    }
    case N_NEW: {
        JVal fn=eval(I,n->a,env);
        JVal args[32]; int ac=n->nlist>32?32:n->nlist;
        for(int i=0;i<ac;i++) args[i]=eval(I,n->list[i],env);
        JObj *o=new_obj(I,K_OBJ); if(!o)return jundef();
        if (fn.t==T_OBJ && (fn.u.obj->kind==K_FUN||fn.u.obj->kind==K_NATIVE)) { JVal r=call_fn(I,fn.u.obj,jobj(o),args,ac); if(r.t==T_OBJ)return r; }
        return jobj(o);
    }
    case N_UNARY: {
        if (n->str && strcmp(n->str,"typeof")==0) { JVal v=eval(I,n->a,env); return jstr(arena_str(&I->a,type_name(v),strlen(type_name(v)))); }
        JVal v=eval(I,n->a,env);
        switch(n->op){
            case OP_NOT: return jbool(!truthy(v));
            case OP_MINUS: return jnum(-to_num(v));
            case OP_PLUS: return jnum(to_num(v));
            case OP_TILDE: return jnum(~to_num(v));
        }
        return jundef();
    }
    case N_UPDATE: {
        JVal old=eval(I,n->a,env); int64_t ov=to_num(old);
        int64_t nv = n->op==OP_INC? ov+1: ov-1;
        assign_to(I,n->a,jnum(nv),env);
        return jnum(n->prefix? nv: ov);
    }
    case N_LOGIC: {
        JVal a=eval(I,n->a,env);
        if (n->op==OP_AND) return truthy(a)? eval(I,n->b,env): a;
        else               return truthy(a)? a: eval(I,n->b,env);
    }
    case N_BIN: {
        JVal a=eval(I,n->a,env), b=eval(I,n->b,env);
        switch(n->op){
            case OP_PLUS:
                if (a.t==T_STR||b.t==T_STR) return jstr(str_concat(I,val_to_str(I,a),val_to_str(I,b)));
                return jnum(to_num(a)+to_num(b));
            case OP_MINUS: return jnum(to_num(a)-to_num(b));
            case OP_STAR:  return jnum(to_num(a)*to_num(b));
            case OP_SLASH: { int64_t d=to_num(b); return jnum(d? to_num(a)/d : 0); }
            case OP_PCT:   { int64_t d=to_num(b); return jnum(d? to_num(a)%d : 0); }
            case OP_SHL: return jnum(to_num(a)<<(to_num(b)&63));
            case OP_SHR: return jnum(to_num(a)>>(to_num(b)&63));
            case OP_AMP: return jnum(to_num(a)&to_num(b));
            case OP_PIPE:return jnum(to_num(a)|to_num(b));
            case OP_CARET:return jnum(to_num(a)^to_num(b));
            case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                if (a.t==T_STR&&b.t==T_STR){ int c=strcmp(a.u.str,b.u.str);
                    return jbool(n->op==OP_LT?c<0:n->op==OP_GT?c>0:n->op==OP_LE?c<=0:c>=0); }
                int64_t x=to_num(a),y=to_num(b);
                return jbool(n->op==OP_LT?x<y:n->op==OP_GT?x>y:n->op==OP_LE?x<=y:x>=y);
            }
            case OP_EQ: return jbool(loose_eq(I,a,b));
            case OP_NEQ:return jbool(!loose_eq(I,a,b));
            case OP_SEQ:return jbool(strict_eq(a,b));
            case OP_SNEQ:return jbool(!strict_eq(a,b));
        }
        return jundef();
    }
    case N_ASSIGN: {
        JVal rhs;
        if (n->op==OP_ASSIGN) rhs=eval(I,n->b,env);
        else {
            JVal cur=eval(I,n->a,env), r=eval(I,n->b,env);
            switch(n->op){
                case OP_PLUSEQ: rhs=(cur.t==T_STR||r.t==T_STR)? jstr(str_concat(I,val_to_str(I,cur),val_to_str(I,r))): jnum(to_num(cur)+to_num(r)); break;
                case OP_MINUSEQ: rhs=jnum(to_num(cur)-to_num(r)); break;
                case OP_STAREQ: rhs=jnum(to_num(cur)*to_num(r)); break;
                case OP_SLASHEQ: { int64_t d=to_num(r); rhs=jnum(d?to_num(cur)/d:0); } break;
                case OP_PCTEQ: { int64_t d=to_num(r); rhs=jnum(d?to_num(cur)%d:0); } break;
                default: rhs=r;
            }
        }
        assign_to(I,n->a,rhs,env);
        return rhs;
    }

    /* statements */
    case N_BLOCK: {
        for(int i=0;i<n->nlist;i++){ eval(I,n->list[i],env); if(I->flow||I->errflag)break; }
        return jundef();
    }
    case N_VAR: {
        Node **inits=(Node**)n->c;
        for(int i=0;i<n->nlist;i++){
            JVal v = inits[i]? eval(I,inits[i],env): jundef();
            env_def(I,env,n->list[i]->str,v);
        }
        return jundef();
    }
    case N_FUNDECL: {
        JObj *o=new_obj(I,K_FUN); if(!o)return jundef();
        o->params=(Node*)n; o->body=n->a; o->closure=env;
        env_def(I,env,n->str,jobj(o));
        return jundef();
    }
    case N_EXPR: return eval(I,n->a,env);
    case N_EMPTY: return jundef();
    case N_IF: if(truthy(eval(I,n->a,env))) eval(I,n->b,env); else if(n->c) eval(I,n->c,env); return jundef();
    case N_WHILE: {
        while (truthy(eval(I,n->a,env))) {
            eval(I,n->d?n->d:n->b,env);
            if (I->flow==1||I->errflag) break;
            if (I->flow==2){ I->flow=0; break; }
            if (I->flow==3) I->flow=0;
            if (++I->steps>50000000){ seterr(I,"loop too long"); break; }
        }
        return jundef();
    }
    case N_FOR: {
        JEnv *fe=new_env(I,env);
        if (n->a) eval(I,n->a,fe);
        while (!n->b || truthy(eval(I,n->b,fe))) {
            eval(I,n->d,fe);
            if (I->flow==1||I->errflag) break;
            if (I->flow==2){ I->flow=0; break; }
            if (I->flow==3) I->flow=0;
            if (n->c) eval(I,n->c,fe);
            if (++I->steps>50000000){ seterr(I,"loop too long"); break; }
        }
        return jundef();
    }
    case N_FORIN: {
        JVal obj=eval(I,n->b,env);
        JEnv *fe=new_env(I,env);
        if (obj.t==T_OBJ) {
            JObj *o=obj.u.obj;
            if (o->kind==K_ARR) {
                for(int i=0;i<o->len;i++){ env_def(I,fe,n->a->str,jnum(i)); eval(I,n->c,fe);
                    if(I->flow==1||I->errflag)break; if(I->flow==2){I->flow=0;break;} if(I->flow==3)I->flow=0; }
            } else {
                for(Prop *p=o->props;p;p=p->next){ env_def(I,fe,n->a->str,jstr(p->key)); eval(I,n->c,fe);
                    if(I->flow==1||I->errflag)break; if(I->flow==2){I->flow=0;break;} if(I->flow==3)I->flow=0; }
            }
        }
        return jundef();
    }
    case N_RET: { I->retval=n->a? eval(I,n->a,env): jundef(); I->flow=1; return jundef(); }
    case N_BREAK: I->flow=2; return jundef();
    case N_CONT: I->flow=3; return jundef();
    }
    return jundef();
}

static JVal call_fn(Interp *I, JObj *fn, JVal thisv, JVal *args, int nargs) {
    if (fn->kind==K_NATIVE) return fn->nat(I, fn->dom?fn->bound_this:thisv, args, nargs);
    if (++I->steps>50000000){ seterr(I,"too deep"); return jundef(); }
    Node *def=fn->params;                 /* the N_FUN/N_FUNDECL node */
    JEnv *fe=new_env(I,fn->closure);
    for(int i=0;i<def->nlist;i++) env_def(I,fe,def->list[i]->str, i<nargs?args[i]:jundef());
    /* arguments array */
    JObj *ar=new_obj(I,K_ARR); if(ar){ for(int i=0;i<nargs;i++)arr_push(I,ar,args[i]); env_def(I,fe,"arguments",jobj(ar)); }
    env_def(I,fe,"this",thisv);
    int saved=I->flow; I->flow=0; I->retval=jundef();
    eval(I,fn->body,fe);
    JVal r = (I->flow==1)? I->retval: jundef();
    I->flow=saved;
    return r;
}

#include "js_builtins.h"

/* ----------------------------- public API ------------------------------ */
int js_run(const char *src, uint32_t len, js_host *host, char *err, uint32_t errcap) {
    Interp *I=(Interp*)kmalloc(sizeof(Interp));
    if (!I) { if(err&&errcap)err[0]=0; return -1; }
    memset(I,0,sizeof(*I));
    I->src=src; I->slen=len; I->host=host;
    I->global=new_env(I,0);
    js_install_globals(I);

    /* parse + execute top-level statements */
    while (peek(I)->k!=TK_EOF && !I->errflag) {
        Node *s=parse_stmt(I);
        if (!s) break;
        eval(I,s,I->global);
        I->flow=0;
        if (I->a.oom){ seterr(I,"out of memory"); break; }
    }
    js_drain_micro(I);                 /* run fetch()/Promise reactions */

    int rc = I->errflag? -1: 0;
    if (err && errcap) { uint32_t i=0; for(;I->err[i]&&i<errcap-1;i++)err[i]=I->err[i]; err[i]=0; }
    arena_free(&I->a);
    kfree(I);
    return rc;
}

/* ===========================================================================
 *  Persistent VM: keep the interpreter (heap + global env + registered timers
 *  and event listeners) alive across calls so handlers can fire later.
 * ===========================================================================*/
struct js_vm { Interp I; };

js_vm *js_vm_create(js_host *host) {
    js_vm *vm = (js_vm*)kmalloc(sizeof(js_vm));
    if (!vm) return 0;
    memset(vm, 0, sizeof(*vm));
    vm->I.host = host;
    vm->I.global = new_env(&vm->I, 0);
    js_install_globals(&vm->I);
    return vm;
}

int js_vm_eval(js_vm *vm, const char *src, uint32_t len, char *err, uint32_t errcap) {
    if (!vm) return -1;
    Interp *I = &vm->I;
    I->src = src; I->slen = len; I->pos = 0; I->has_peek = 0;
    I->errflag = 0; I->flow = 0; I->retval = jundef();
    while (peek(I)->k != TK_EOF && !I->errflag) {
        Node *s = parse_stmt(I); if (!s) break;
        eval(I, s, I->global); I->flow = 0;
        if (I->a.oom) { seterr(I, "out of memory"); break; }
    }
    js_drain_micro(I);                 /* run fetch()/Promise reactions */
    int rc = I->errflag ? -1 : 0;
    if (err && errcap) { uint32_t i=0; for(;I->err[i]&&i<errcap-1;i++)err[i]=I->err[i]; err[i]=0; }
    return rc;
}

int js_vm_pump(js_vm *vm, uint64_t now_ms) {
    if (!vm) return 0;
    Interp *I = &vm->I; I->now_ms = now_ms;
    int fired = 0;
    for (int i = 0; i < I->ntimers; i++) {
        if (!I->timers[i].active || I->timers[i].due > now_ms) continue;
        JObj *fn = I->timers[i].fn;
        if (I->timers[i].interval) I->timers[i].due = now_ms + I->timers[i].interval;
        else I->timers[i].active = 0;
        I->errflag = 0; I->flow = 0;
        call_fn(I, fn, jundef(), 0, 0);
        fired++;
        if (I->a.oom) break;
    }
    js_drain_micro(I);                 /* timers may have scheduled promise work */
    return fired;
}

int js_vm_dispatch(js_vm *vm, js_dom_node target, const char *type) {
    if (!vm) return 0;
    Interp *I = &vm->I; int n = 0;
    for (int i = 0; i < I->nlisteners; i++) {
        if (!I->listeners[i].active) continue;
        if (strcmp(I->listeners[i].type, type) != 0) continue;
        if (I->listeners[i].target != JS_TARGET_DOC && I->listeners[i].target != target) continue;
        /* event arg: { type, target } */
        JObj *ev = new_obj(I, K_OBJ);
        if (ev) { obj_set(I, ev, "type", jstr(arena_str(&I->a, type, strlen(type))));
                  if (target) obj_set(I, ev, "target", wrap_dom(I, target)); }
        JVal arg = ev ? jobj(ev) : jundef();
        JVal thisv = target ? wrap_dom(I, target) : jundef();
        I->errflag = 0; I->flow = 0;
        call_fn(I, I->listeners[i].fn, thisv, &arg, 1);
        n++;
        if (I->a.oom) break;
    }
    js_drain_micro(I);                 /* handlers may have called fetch()/then */
    return n;
}

int js_vm_has_work(js_vm *vm) {
    if (!vm) return 0;
    Interp *I = &vm->I;
    if (I->mq_count) return 1;
    for (int i = 0; i < I->ntimers; i++) if (I->timers[i].active) return 1;
    return I->nlisteners > 0;
}

void js_vm_destroy(js_vm *vm) {
    if (!vm) return;
    arena_free(&vm->I.a);
    kfree(vm);
}
