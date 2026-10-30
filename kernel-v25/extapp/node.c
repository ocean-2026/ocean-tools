/*
 * node.ce -- CatOS "node" subset: a tiny JavaScript interpreter.
 *
 * Runs a JS script with the core subset: variables (var/let), functions
 * (params/return/recursion), loops (for/while/break/continue), arrays
 * (literal, index, .length, .push/.pop), objects (literal, member read/
 * write, nested, methods with `this`), strings, integer arithmetic,
 * comparisons, logical operators, comments, typeof / print / console.log.
 *
 * Integers only (the CE build has no FPU; -mno-80387), arithmetic is
 * 32-bit truncating, `/` truncates like C.
 *
 * Script source selection (first match wins):
 *   1. argv[1]  -> run /bin/node.ce /PATH/TO/SCRIPT.JS
 *   2. /BIN/DEMO.JS exists on the volume -> executed as the script
 *   3. otherwise the embedded demo below runs
 *
 * Build: sh extapp/build_ce.sh node.c   -> extapp/bin/node.ce
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* ====================================================================
 * tiny freestanding helpers (no libc)
 * ================================================================== */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void scpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void sncpy(char *d, const char *s, int n) {
    while (n-- > 0 && *s) *d++ = *s++;
    *d = 0;
}
static int str_eqn(const char *a, const char *b, int nb) {   /* a[0..nb)==b */
    while (nb-- > 0) if (*a++ != *b++) return 0;
    return 1;                       /* token boundary already exact */
}
/* "123" -> 123 ; stops at first non-digit; returns 0 on empty */
static int atoi_n(const char *s, int *out) {
    int v = 0, n = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; n++; }
    *out = v; return n;
}

/* ====================================================================
 * arena memory (bump allocator; the interpreter runs one script and
 * exits, so freeing is unnecessary -- memory use is bounded by the
 * script + interpreter data)
 * ================================================================== */
#define ARENA_SZ (160u * 1024u)
static uint8_t g_arena[ARENA_SZ];
static uint32_t g_apos;
static int g_oom;                      /* set when arena is exhausted */

static void *a_alloc(uint32_t n) {
    n = (n + 7u) & ~7u;
    if (g_apos + n > ARENA_SZ) {
        g_oom = 1;
#ifdef NODE_DEBUG
        {
            catos_api_t *api = CE_API();
            api->write("a_alloc OOM: req="); api->print_int((int)n);
            api->write(" apos="); api->print_int((int)g_apos);
            api->write(" sz="); api->print_int((int)ARENA_SZ); api->write("\n");
        }
#endif
        return 0;
    }
    void *p = g_arena + g_apos;
    g_apos += n;
    return p;
}
static char *a_str(const char *s) {
    int n = slen(s) + 1;
    char *p = (char *)a_alloc((uint32_t)n);
    if (p) sncpy(p, s, n);
    return p;
}
static char *a_strn(const char *s, int n) {
    char *p = (char *)a_alloc((uint32_t)n + 1);
    if (p) sncpy(p, s, n);
    return p;
}

/* ====================================================================
 * values
 * ================================================================== */
enum {
    V_UNDEF, V_NUM, V_STR, V_BOOL, V_NULL, V_FUNC, V_OBJ, V_ARR
};
typedef struct JSVal {
    uint8_t  type;
    uint8_t  rsvd;
    int32_t  num;         /* number value / builtin id / bool */
    const char *sp;       /* string data / object key / func name */
    int32_t  slen;        /* string length */
    void    *op;          /* object / array / function pointer */
} JSVal;

typedef struct VarE { const char *name; JSVal v; } VarE;
typedef struct JSObj { VarE *v; int n, cap; } JSObj;
typedef struct JSArr { JSVal *v; int n, cap; } JSArr;
typedef struct Node Node;
typedef struct Scope Scope;
typedef struct JSFunc {
    const char *name;
    Node *params, *body;
    Scope *scope;         /* defining scope (closures + recursion) */
    int bid;              /* builtin id, -1 = user function */
} JSFunc;

static const JSVal VAL_UNDEF = { V_UNDEF, 0, 0, 0, 0, 0 };
static const JSVal VAL_NULL  = { V_NULL, 0, 0, 0, 0, 0 };
static const JSVal VAL_TRUE  = { V_BOOL, 0, 1, 0, 0, 0 };
static const JSVal VAL_FALSE = { V_BOOL, 0, 0, 0, 0, 0 };

static JSVal v_num(int32_t n) { JSVal v = { V_NUM, 0, n, 0, 0, 0 }; return v; }
static JSVal v_bool(int b) { return b ? VAL_TRUE : VAL_FALSE; }
static JSVal v_str(const char *p, int n) {
    JSVal v = { V_STR, 0, 0, p, n, 0 }; return v;
}

/* ====================================================================
 * AST
 * ================================================================== */
enum {   /* statements */
    S_BLOCK, S_VAR, S_IF, S_WHILE, S_FOR, S_BREAK, S_CONT, S_RETURN,
    S_EXPR, S_FUNC, S_EMPTY
};
enum {   /* expressions */
    E_NUM, E_STR, E_ID, E_BOOL, E_NULL, E_UNDEF, E_DECL, E_ASSIGN, E_BIN,
    E_UN, E_CALL, E_MEMBER, E_ARR, E_OBJ, E_OBJKEY, E_PRE, E_POST
};
enum {   /* E_BIN operator codes (char values, distinct from keywords) */
    OP_ADD = '+', OP_SUB = '-', OP_MUL = '*', OP_DIV = '/', OP_MOD = '%',
    OP_EQ = 1, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_AND, OP_OR
};

struct Node {
    uint8_t op;
    uint8_t flag;         /* E_MEMBER: 1 = index, 0 = dot; E_DECL: has init */
    int32_t num;          /* literal value / operator */
    const char *sp;       /* string literal / identifier / object key */
    Node *a, *b, *c;      /* children (semantics vary, see parser) */
    Node *body;           /* statement list (block / function / loops) */
    Node *next;           /* statement list link / declarator / args */
};

static Node *n_new(int op) {
    Node *n = (Node *)a_alloc(sizeof(Node));
    if (n) {
        n->op = (uint8_t)op;
        n->flag = 0; n->num = 0; n->sp = 0;
        n->a = n->b = n->c = 0;
        n->body = 0; n->next = 0;       /* zero-init: list tails must be NULL */
    }
    return n;
}
static Node *n_list(Node *n, Node *next) { if (n) n->next = next; return n; }

/* ====================================================================
 * lexer
 * ================================================================== */
enum { T_NUM, T_ID, T_STR, T_OP, T_EOF };
typedef struct Tok {
    uint8_t type;
    int32_t num;
    const char *sp;
    int len;
} Tok;

#define TOK_MAX 2048
static Tok g_tok[TOK_MAX];
static int g_tokn;

static const char *g_src;
static int g_pos;
static int g_slen;

static int is_idc(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }

static void lex_add(int type, const char *sp, int len, int32_t num) {
    if (g_tokn >= TOK_MAX) return;          /* truncate: parser errors out */
    Tok *t = &g_tok[g_tokn++];
    t->type = (uint8_t)type; t->sp = sp; t->len = len; t->num = num;
}
static int lex_skip_ws(void) {
    for (;;) {
        int c = g_src[g_pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { g_pos++; continue; }
        if (c == '/' && g_src[g_pos + 1] == '/') {           /* line comment */
            while (g_src[g_pos] && g_src[g_pos] != '\n') g_pos++;
            continue;
        }
        if (c == '/' && g_src[g_pos + 1] == '*') {           /* block comment */
            g_pos += 2;
            while (g_src[g_pos] && !(g_src[g_pos] == '*' && g_src[g_pos + 1] == '/'))
                g_pos++;
            if (g_src[g_pos]) g_pos += 2;
            continue;
        }
        return c;
    }
}

static void lex_run(const char *src, int len) {
    g_src = src; g_slen = len; g_pos = 0; g_tokn = 0;
    for (;;) {
        int c = lex_skip_ws();
        if (!c || g_pos >= g_slen) { lex_add(T_EOF, src + g_pos, 0, 0); break; }
        if (is_digit(c)) {
            const char *sp = src + g_pos;
            int v = 0;
            while (is_digit(src[g_pos])) { v = v * 10 + (src[g_pos] - '0'); g_pos++; }
            lex_add(T_NUM, sp, g_pos - (int)(sp - src), v);
        } else if (is_idc(c)) {
            const char *sp = src + g_pos;
            while (is_idc(src[g_pos])) g_pos++;
            lex_add(T_ID, sp, g_pos - (int)(sp - src), 0);
        } else if (c == '"' || c == '\'') {
            int q = c; g_pos++;
            const char *sp = src + g_pos;
            /* build the unescaped string in the arena */
            int cap = 64, n = 0;
            char *buf = (char *)a_alloc(64);
            if (!buf) { lex_add(T_EOF, src + g_pos, 0, 0); break; }
            for (;;) {
                int ch = src[g_pos];
                if (!ch) break;
                g_pos++;
                if (ch == q) break;
                if (ch == '\\') {
                    int e = src[g_pos];
                    if (e) g_pos++;
                    ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : e;
                }
                if (n + 1 >= cap) { cap *= 2; char *nb = (char *)a_alloc((uint32_t)cap); if (!nb) break; for (int i = 0; i < n; i++) nb[i] = buf[i]; buf = nb; }
                buf[n++] = (char)ch;
            }
            buf[n] = 0;
            char *keep = (char *)a_alloc((uint32_t)n + 1);
            if (keep) { for (int i = 0; i <= n; i++) keep[i] = buf[i]; }
            lex_add(T_STR, keep ? keep : sp, n, 0);
        } else {
            /* operators (longest match first) */
            const char *sp = src + g_pos;
            int op = c;
            if (c == '=' || c == '!' || c == '<' || c == '>' || c == '+' ||
                c == '-' || c == '*' || c == '/' || c == '%' || c == '&' || c == '|') {
                int c2 = src[g_pos + 1];
                if (c2 == '=' || (c == '&' && c2 == '&') || (c == '|' && c2 == '|')) {
                    op = (c == '&') ? -1 : (c == '|') ? -2 :
                         (c == '=' && c2 == '=') ? '=' : c;   /* '==' stays '=' */
                    g_pos += 2;
                    lex_add(T_OP, sp, 2, op);
                    continue;
                }
                if (c == '+' && c2 == '+') { g_pos += 2; lex_add(T_OP, sp, 2, 'P'); continue; }
                if (c == '-' && c2 == '-') { g_pos += 2; lex_add(T_OP, sp, 2, 'M'); continue; }
                if (c == '=' && c2 == '=') { g_pos += 2; lex_add(T_OP, sp, 2, '='); continue; }
                if ((c == '&' || c == '|') ) { op = (c == '&') ? -1 : -2; g_pos += 1; lex_add(T_OP, sp, 1, op); continue; }
            }
            g_pos++;
            lex_add(T_OP, sp, 1, op);
        }
    }
}

/* ====================================================================
 * parser (recursive descent over g_tok[])
 * ================================================================== */
static int g_ti;
static int g_perr;

static int tok_is(int type) { return g_tok[g_ti].type == type; }
static int tok_op(int op) { return g_tok[g_ti].type == T_OP && g_tok[g_ti].num == op; }
static int tok_kw(const char *kw) {
    int klen = 0;
    while (kw[klen]) klen++;
    return g_tok[g_ti].type == T_ID && g_tok[g_ti].len == klen &&
           str_eqn(g_tok[g_ti].sp, kw, klen);
}
static void tadv(void) { if (g_ti < g_tokn - 1) g_ti++; }

static void perr(const char *msg) {
    (void)msg;
    if (!g_perr) {
        g_perr = 1;
#ifdef NODE_DEBUG
        {
            catos_api_t *api = CE_API();
            api->write("perr#"); api->print_int(g_ti); api->putc(' ');
            if (g_tok[g_ti].type == T_ID) { api->write("id="); for (int k = 0; k < g_tok[g_ti].len && k < 20; k++) api->putc(g_tok[g_ti].sp[k]); }
            else if (g_tok[g_ti].type == T_OP) { api->write("op="); api->putc((char)g_tok[g_ti].num); api->write(" len="); api->print_int(g_tok[g_ti].len); }
            else api->print_int(g_tok[g_ti].type);
            api->write(" <"); api->write(msg); api->write(">\n");
        }
#endif
    }
}
static Node *p_stmt(void);
static Node *p_expr(void);

static Node *p_primary(void) {
    Tok *t = &g_tok[g_ti];
    Node *n = 0;
    if (t->type == T_NUM) { n = n_new(E_NUM); if (n) n->num = t->num; tadv(); return n; }
    if (t->type == T_STR) { n = n_new(E_STR); if (n) { n->sp = t->sp; n->num = t->len; } tadv(); return n; }
    if (t->type == T_ID) {
        if (tok_kw("true"))  { tadv(); n = n_new(E_BOOL); if (n) n->num = 1; return n; }
        if (tok_kw("false")) { tadv(); n = n_new(E_BOOL); if (n) n->num = 0; return n; }
        if (tok_kw("null"))  { tadv(); n = n_new(E_NULL); return n; }
        if (tok_kw("undefined")) { tadv(); n = n_new(E_UNDEF); return n; }
        n = n_new(E_ID);
        if (n) {
            n->sp = a_strn(t->sp, t->len);
        }
        tadv(); return n;
    }
    if (tok_op('(')) {
        tadv();
        n = p_expr();
        if (!tok_op(')')) { perr("expected )"); }
        else tadv();
        return n;
    }
    if (tok_op('[')) {
        tadv();
        n = n_new(E_ARR);
        Node *tail = 0;
        if (!tok_op(']')) {
            for (;;) {
                Node *e = p_expr();
                if (!e) break;
                if (tail) tail->next = e; else if (n) n->a = e;
                tail = e;
                if (tok_op(',')) tadv();
                else break;
            }
        }
        if (!tok_op(']')) perr("expected ]");
        else tadv();
        return n;
    }
    if (tok_op('{')) {
        tadv();
        n = n_new(E_OBJ);
        Node *tail = 0;
        if (!tok_op('}')) {
            for (;;) {
                Node *k = n_new(E_OBJKEY);
                if (g_tok[g_ti].type == T_ID || g_tok[g_ti].type == T_STR) {
                    if (k) k->sp = (g_tok[g_ti].type == T_ID)
                                 ? a_strn(g_tok[g_ti].sp, g_tok[g_ti].len)
                                 : g_tok[g_ti].sp;
                } else { perr("bad object key"); }
                tadv();
                if (!tok_op(':')) perr("expected :");
                else tadv();
                if (k) k->a = p_expr();
                if (tail) tail->next = k; else if (n) n->a = k;
                tail = k;
                if (tok_op(',')) tadv();
                else break;
            }
        }
        if (!tok_op('}')) perr("expected }");
        else tadv();
        return n;
    }
    perr("unexpected token");
    return 0;
}

static Node *p_postfix(void) {
    Node *n = p_primary();
    if (!n) return 0;
    for (;;) {
        if (tok_op('.')) {
            tadv();
            if (g_tok[g_ti].type != T_ID) { perr("expected member name"); break; }
            Node *m = n_new(E_MEMBER);
            if (m) { m->a = n; m->flag = 0; m->b = n_new(E_STR); if (m->b) { m->b->sp = a_strn(g_tok[g_ti].sp, g_tok[g_ti].len); } }
            n = m;
            tadv();
        } else if (tok_op('[')) {
            tadv();
            Node *idx = p_expr();
            if (!tok_op(']')) perr("expected ]"); else tadv();
            Node *m = n_new(E_MEMBER);
            if (m) { m->a = n; m->flag = 1; m->b = idx; }
            n = m;
        } else if (tok_op('(')) {
            tadv();
            Node *args = 0, *tail = 0;
            if (!tok_op(')')) {
                for (;;) {
                    Node *e = p_expr();
                    if (tail) tail->next = e; else args = e;
                    tail = e;
                    if (tok_op(',')) tadv();
                    else break;
                }
            }
            if (!tok_op(')')) perr("expected )"); else tadv();
            Node *c = n_new(E_CALL);
            if (c) { c->a = n; c->b = args; }
            n = c;
        } else if (tok_op('P')) { tadv(); Node *u = n_new(E_POST); if (u) { u->a = n; u->num = 1; } n = u; }
        else if (tok_op('M')) { tadv(); Node *u = n_new(E_POST); if (u) { u->a = n; u->num = -1; } n = u; }
        else break;
    }
    return n;
}

static Node *p_unary(void) {
    if (tok_op('!')) { tadv(); Node *u = n_new(E_UN); if (u) { u->num = '!'; u->a = p_unary(); } return u; }
    if (tok_op('-')) { tadv(); Node *u = n_new(E_UN); if (u) { u->num = '-'; u->a = p_unary(); } return u; }
    if (tok_op('P')) { tadv(); Node *u = n_new(E_PRE); if (u) { u->num = 1; u->a = p_unary(); } return u; }
    if (tok_op('M')) { tadv(); Node *u = n_new(E_PRE); if (u) { u->num = -1; u->a = p_unary(); } return u; }
    if (tok_kw("typeof")) {           /* typeof x  (paren form parses as call) */
        tadv();
        Node *c = n_new(E_CALL);
        if (c) {
            Node *id = n_new(E_ID);
            if (id) id->sp = a_strn("typeof", 6);
            c->a = id;
            c->b = p_unary();
        }
        return c;
    }
    return p_postfix();
}

static Node *p_bin(int min_prio) {
    Node *l = 0;
    /* mult/additive/relational/equality/and/or */
    int prio;
    if (min_prio <= 4) l = p_bin(5); else l = p_unary();
    if (!l) return 0;
    for (;;) {
        int op = 0;
        if      (tok_op('*') && g_tok[g_ti].len == 1)    prio = 5, op = (int)g_tok[g_ti].num;
        else if (tok_op('/') && g_tok[g_ti].len == 1)    prio = 5, op = (int)g_tok[g_ti].num;
        else if (tok_op('%') && g_tok[g_ti].len == 1)    prio = 5, op = (int)g_tok[g_ti].num;
        else if (tok_op('+') && g_tok[g_ti].len == 1)    prio = 4, op = (int)g_tok[g_ti].num;
        else if (tok_op('-') && g_tok[g_ti].len == 1)    prio = 4, op = (int)g_tok[g_ti].num;
        else if (tok_op('<') && g_tok[g_ti].len == 1)    prio = 3, op = OP_LT;
        else if (tok_op('>') && g_tok[g_ti].len == 1)    prio = 3, op = OP_GT;
        else if (tok_op('=') && (g_tok[g_ti].len == 1))  { prio = 0; break; }   /* single '=' handled by assign */
        else if (tok_op('=') && (g_tok[g_ti].len == 2))  prio = 2, op = OP_EQ;
        else if (tok_op('!') && (g_tok[g_ti].len == 2))  prio = 2, op = OP_NE;
        else if (tok_op('<') && (g_tok[g_ti].len == 2))  prio = 3, op = OP_LE;
        else if (tok_op('>') && (g_tok[g_ti].len == 2))  prio = 3, op = OP_GE;
        else if (tok_op(-1))                             prio = 1, op = OP_AND;
        else if (tok_op(-2))                             prio = 1, op = OP_OR;
        else break;
        if (prio < min_prio) break;
        tadv();
        Node *r = p_bin(prio + 1);
        Node *b = n_new(E_BIN);
        if (b) { b->a = l; b->b = r; b->num = op; }
        l = b;
    }
    return l;
}

/* assignment: a = expr / += -= *= /= %=  (right assoc) */
static Node *p_assign(void) {
    Node *l = p_bin(1);
    if (!l) return 0;
    int op = 0;
    if (g_tok[g_ti].type == T_OP) {
        int c = (int)g_tok[g_ti].num, len = g_tok[g_ti].len;
        if (c == '=' && len == 1) op = '=';
        else if (c == '+' && len == 2) op = OP_ADD;
        else if (c == '-' && len == 2) op = OP_SUB;
        else if (c == '*' && len == 2) op = OP_MUL;
        else if (c == '/' && len == 2) op = OP_DIV;
        else if (c == '%' && len == 2) op = OP_MOD;
    }
    if (!op) return l;
    tadv();
    Node *r = p_assign();
    Node *a = n_new(E_ASSIGN);
    if (a) { a->a = l; a->b = r; a->num = op; }
    return a;
}

static Node *p_expr(void) { return p_assign(); }

/* statement list: parse into a next-chain; returns head */
static Node *p_stmt_list(void) {
    Node *head = 0, *tail = 0;
    for (;;) {
        if (g_ti >= g_tokn || tok_is(T_EOF) || tok_op('}')) break;
        int ti0 = g_ti;
        Node *s = p_stmt();
        if (!s) break;
        if (tail) tail->next = s; else head = s;
        tail = s;
        if (g_ti == ti0) tadv();       /* error recovery: force progress */
    }
    return head;
}

static Node *p_stmt(void) {
    if (tok_kw("var") || tok_kw("let")) {
        tadv();
        Node *n = n_new(S_VAR);
        Node *tail = 0;
        for (;;) {
            if (g_tok[g_ti].type != T_ID) { perr("expected variable name"); break; }
            Node *d = n_new(E_DECL);
            if (d) {
                d->sp = a_strn(g_tok[g_ti].sp, g_tok[g_ti].len);
                tadv();
                if (tok_op('=')) { tadv(); d->flag = 1; d->a = p_expr(); }
            } else tadv();
            if (tail) tail->next = d; else if (n) n->a = d;
            tail = d;
            if (tok_op(',')) tadv();
            else break;
        }
        if (!tok_op(';')) perr("expected ;");
        else tadv();
        return n;
    }
    if (tok_kw("function")) {
        tadv();
        if (g_tok[g_ti].type != T_ID) { perr("expected function name"); return 0; }
        const char *fname = a_strn(g_tok[g_ti].sp, g_tok[g_ti].len);
        tadv();
        Node *n = n_new(S_FUNC);
        if (!n) return 0;
        n->sp = fname;
        if (!tok_op('(')) { perr("expected ("); return n; }
        tadv();
        Node *ptail = 0;
        if (!tok_op(')')) {
            for (;;) {
                if (g_tok[g_ti].type != T_ID) { perr("bad param"); break; }
                Node *p = n_new(E_ID);
                if (p) p->sp = a_strn(g_tok[g_ti].sp, g_tok[g_ti].len);
                if (ptail) ptail->next = p; else n->a = p;
                ptail = p;
                tadv();
                if (tok_op(',')) tadv();
                else break;
            }
        }
        if (!tok_op(')')) perr("expected )");
        else tadv();
        if (!tok_op('{')) { perr("expected {"); return n; }
        tadv();
        n->body = p_stmt_list();
        if (!tok_op('}')) perr("expected }");
        else tadv();
        return n;
    }
    if (tok_kw("if")) {
        tadv();
        Node *n = n_new(S_IF);
        if (!tok_op('(')) perr("expected ("); else tadv();
        if (n) n->a = p_expr();
        if (!tok_op(')')) perr("expected )"); else tadv();
        if (n) n->b = p_stmt();
        if (tok_kw("else")) { tadv(); if (n) n->c = p_stmt(); }
        return n;
    }
    if (tok_kw("while")) {
        tadv();
        Node *n = n_new(S_WHILE);
        if (!tok_op('(')) perr("expected ("); else tadv();
        if (n) n->a = p_expr();
        if (!tok_op(')')) perr("expected )"); else tadv();
        if (n) n->body = p_stmt();
        return n;
    }
    if (tok_kw("for")) {
        tadv();
        Node *n = n_new(S_FOR);
        if (!tok_op('(')) perr("expected ("); else tadv();
        if (tok_op(';')) { tadv(); }
        else if (tok_kw("var") || tok_kw("let")) { if (n) n->a = p_stmt(); }
        else { if (n) n->a = p_expr(); if (!tok_op(';')) perr("expected ;"); else tadv(); }
        if (!tok_op(';')) {
            if (n) n->b = p_expr();
            if (!tok_op(';')) perr("expected ;"); else tadv();
        }
        if (!tok_op(')')) { if (n) n->c = p_expr(); }
        if (!tok_op(')')) perr("expected )"); else tadv();
        if (n) n->body = p_stmt();
        return n;
    }
    if (tok_kw("break"))   { tadv(); if (!tok_op(';')) perr("expected ;"); else tadv(); return n_new(S_BREAK); }
    if (tok_kw("continue")){ tadv(); if (!tok_op(';')) perr("expected ;"); else tadv(); return n_new(S_CONT); }
    if (tok_kw("return"))  {
        tadv();
        Node *n = n_new(S_RETURN);
        if (!tok_op(';')) { if (n) n->a = p_expr(); }
        if (!tok_op(';')) perr("expected ;"); else tadv();
        return n;
    }
    if (tok_op('{')) {
        tadv();
        Node *n = n_new(S_BLOCK);
        if (n) n->body = p_stmt_list();
        if (!tok_op('}')) perr("expected }"); else tadv();
        return n;
    }
    if (tok_op(';')) { tadv(); return n_new(S_EMPTY); }
    {   /* expression statement */
        Node *n = p_expr();
        if (!tok_op(';')) perr("expected ;");
        else tadv();
        Node *s = n_new(S_EXPR);
        if (s) s->a = n;
        return s;
    }
}

/* ====================================================================
 * scopes
 * ================================================================== */
struct Scope { Scope *parent; VarE *v; int n, cap; };
static Scope *g_scope;          /* innermost */
static Scope *g_globals;

static Scope *scope_new(Scope *parent) {
    Scope *s = (Scope *)a_alloc(sizeof(Scope));
    if (!s) return 0;
    s->parent = parent; s->n = 0; s->cap = 8;
    s->v = (VarE *)a_alloc(sizeof(VarE) * 8u);
    if (!s->v) return 0;
    return s;
}
static void scope_grow(Scope *s) {
    int ncap = s->cap * 2;
    VarE *nv = (VarE *)a_alloc(sizeof(VarE) * (uint32_t)ncap);
    if (!nv) return;
    for (int i = 0; i < s->n; i++) nv[i] = s->v[i];
    s->v = nv; s->cap = ncap;
}
static VarE *scope_find(Scope *s, const char *name) {
    for (; s; s = s->parent)
        for (int i = 0; i < s->n; i++)
            if (scmp(s->v[i].name, name) == 0) return &s->v[i];
    return 0;
}
static VarE *scope_declare(Scope *s, const char *name) {
    VarE *e = scope_find(s, name);
    if (e) return e;
    if (s->n >= s->cap) scope_grow(s);
    if (s->n >= s->cap) return 0;
    s->v[s->n].name = name;
    s->v[s->n].v = VAL_UNDEF;
    return &s->v[s->n++];
}

/* ====================================================================
 * output (each print() writes one line to the 80x25 console)
 * ================================================================== */
static char g_out[256];
static int g_oln;

static void out_reset(void) { g_oln = 0; g_out[0] = 0; }
static void out_pushc(char c) { if (g_oln < (int)sizeof(g_out) - 1) g_out[g_oln++] = c; g_out[g_oln] = 0; }
static void out_pushs(const char *s) { while (*s) out_pushc(*s++); }
static void out_pushn(int32_t v) {
    char tmp[16];
    int neg = v < 0, n = 0;
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) tmp[n++] = '-';
    while (n) out_pushc(tmp[--n]);
}
static void out_flush(catos_api_t *api) {
    out_pushc('\n');
    api->write(g_out);
    out_reset();
}

/* forward */
static JSVal call_builtin(catos_api_t *api, int bid, JSVal *args, int argc);
static JSVal call_user(catos_api_t *api, JSFunc *f, JSVal *args, int argc, JSVal thisv);
static void out_value(catos_api_t *api, JSVal v, int depth);

/* ====================================================================
 * interpreter
 * ================================================================== */
static int g_err;                 /* runtime error set */
static const char *g_errmsg;
static int32_t g_errnum;
static int g_flow;                /* 0 normal, 1 break, 2 continue, 3 return */
static JSVal g_ret;
static int g_depth;

static void rerr(const char *msg) { if (!g_err) { g_err = 1; g_errmsg = msg; g_errnum = 0; } }

static int truthy(JSVal v) {
    switch (v.type) {
    case V_NUM:  return v.num != 0;
    case V_BOOL: return v.num != 0;
    case V_STR:  return v.slen > 0;
    case V_NULL: case V_UNDEF: return 0;
    default: return 1;
    }
}

static JSVal eval_expr(Node *n);
static void exec_stmt(Node *n);
static void exec_list(Node *n);

/* ---- objects ---- */
static JSObj *obj_new(void) {
    JSObj *o = (JSObj *)a_alloc(sizeof(JSObj));
    if (!o) return 0;
    o->n = 0; o->cap = 4;
    o->v = (VarE *)a_alloc(sizeof(VarE) * 4u);
    return o;
}
static void obj_grow(JSObj *o) {
    int ncap = o->cap * 2;
    VarE *nv = (VarE *)a_alloc(sizeof(VarE) * (uint32_t)ncap);
    if (!nv) return;
    for (int i = 0; i < o->n; i++) nv[i] = o->v[i];
    o->v = nv; o->cap = ncap;
}
static VarE *obj_get(JSObj *o, const char *name) {
    for (int i = 0; i < o->n; i++)
        if (scmp(o->v[i].name, name) == 0) return &o->v[i];
    return 0;
}
static void obj_set(JSObj *o, const char *name, JSVal v) {
    VarE *e = obj_get(o, name);
    if (e) { e->v = v; return; }
    if (o->n >= o->cap) obj_grow(o);
    if (o->n >= o->cap) return;
    o->v[o->n].name = name;
    o->v[o->n].v = v;
    o->n++;
}

/* ---- arrays ---- */
static JSArr *arr_new(void) {
    JSArr *a = (JSArr *)a_alloc(sizeof(JSArr));
    if (!a) return 0;
    a->n = 0; a->cap = 4;
    a->v = (JSVal *)a_alloc(sizeof(JSVal) * 4u);
    return a;
}
static void arr_grow(JSArr *a) {
    int ncap = a->cap * 2;
    JSVal *nv = (JSVal *)a_alloc(sizeof(JSVal) * (uint32_t)ncap);
    if (!nv) return;
    for (int i = 0; i < a->n; i++) nv[i] = a->v[i];
    a->v = nv; a->cap = ncap;
}
static void arr_push(JSArr *a, JSVal v) {
    if (a->n >= a->cap) arr_grow(a);
    if (a->n >= a->cap) return;
    a->v[a->n++] = v;
}
static int arr_index(JSArr *a, int32_t i) { return i >= 0 && i < a->n; }

/* ---- functions ---- */
static JSFunc *func_new(const char *name, Node *params, Node *body, Scope *scope) {
    JSFunc *f = (JSFunc *)a_alloc(sizeof(JSFunc));
    if (!f) return 0;
    f->name = name; f->params = params; f->body = body; f->scope = scope; f->bid = -1;
    return f;
}
static JSFunc *bfunc_new(const char *name, int bid) {
    JSFunc *f = (JSFunc *)a_alloc(sizeof(JSFunc));
    if (!f) return 0;
    f->name = name; f->params = 0; f->body = 0; f->scope = 0; f->bid = bid;
    return f;
}

static JSVal call_builtin(catos_api_t *api, int bid, JSVal *args, int argc);

/* ---- assignment target write-back ---- */
static int assign_target(Node *n, JSVal v) {
    if (n->op == E_ID) {
        VarE *e = scope_find(g_scope, n->sp);
        if (!e) e = scope_declare(g_globals, n->sp);   /* sloppy: global */
        if (!e) { rerr("out of memory"); return -1; }
        e->v = v;
        return 0;
    }
    if (n->op == E_MEMBER) {
        JSVal base = eval_expr(n->a);
        if (n->flag == 1) {                     /* arr[i] = v */
            JSVal iv = eval_expr(n->b);
            if (base.type == V_ARR && iv.type == V_NUM && arr_index((JSArr *)base.op, iv.num)) {
                ((JSArr *)base.op)->v[iv.num] = v;
                return 0;
            }
            rerr("bad array index assignment");
            return -1;
        }
        if (base.type == V_OBJ) {               /* obj.key = v */
            obj_set((JSObj *)base.op, n->b ? n->b->sp : 0, v);
            return 0;
        }
        rerr("cannot assign to member");
        return -1;
    }
    rerr("bad assignment target");
    return -1;
}

static JSVal eval_expr(Node *n) {
    if (!n || g_err) return VAL_UNDEF;
    switch (n->op) {
    case E_NUM:  return v_num(n->num);
    case E_STR:  return v_str(n->sp, n->num);
    case E_BOOL: return v_bool(n->num);
    case E_NULL: return VAL_NULL;
    case E_UNDEF:return VAL_UNDEF;
    case E_ID: {
        VarE *e = scope_find(g_scope, n->sp);
        return e ? e->v : VAL_UNDEF;
    }
    case E_DECL: {
        JSVal v = n->flag ? eval_expr(n->a) : VAL_UNDEF;
        VarE *e = scope_declare(g_scope, n->sp);
        if (e) e->v = v;
        return v;
    }
    case E_ASSIGN: {
        JSVal r = eval_expr(n->b);
        if (n->num != '=') {
            JSVal l = eval_expr(n->a);
            if (l.type != V_NUM || r.type != V_NUM) { rerr("+=-op needs numbers"); return VAL_UNDEF; }
            int32_t lv = l.num;
            switch (n->num) {
            case OP_ADD: r = v_num(lv + r.num); break;
            case OP_SUB: r = v_num(lv - r.num); break;
            case OP_MUL: r = v_num(lv * r.num); break;
            case OP_DIV: if (!r.num) { rerr("divide by zero"); return VAL_UNDEF; } r = v_num(lv / r.num); break;
            case OP_MOD: if (!r.num) { rerr("modulo zero"); return VAL_UNDEF; } r = v_num(lv % r.num); break;
            default: rerr("bad compound op"); return VAL_UNDEF;
            }
        }
        if (assign_target(n->a, r) != 0) return VAL_UNDEF;
        return r;
    }
    case E_BIN: {
        if (n->num == OP_AND || n->num == OP_OR || n->num == OP_EQ ||
            n->num == OP_NE || n->num == OP_LT || n->num == OP_LE ||
            n->num == OP_GT || n->num == OP_GE ||
            n->num == OP_ADD || n->num == OP_SUB || n->num == OP_MUL ||
            n->num == OP_DIV || n->num == OP_MOD) { }
        else { g_errnum = n->num; rerr("bad binary op"); return VAL_UNDEF; }
        JSVal l = eval_expr(n->a);
        if (n->num == OP_AND) return truthy(l) ? eval_expr(n->b) : l;
        if (n->num == OP_OR)  return truthy(l) ? l : eval_expr(n->b);
        JSVal r = eval_expr(n->b);
        switch (n->num) {
        case OP_ADD: {
            if (l.type == V_STR || r.type == V_STR) {   /* string concat */
                char buf[512];
                int len = 0;
                if (l.type == V_STR) { for (int i = 0; i < l.slen && len < 500; i++) buf[len++] = l.sp[i]; }
                else { char tmp[16]; int neg = l.num < 0, m = 0; uint32_t u = neg ? (uint32_t)(-(int64_t)l.num) : (uint32_t)l.num;
                       if (!u) tmp[m++] = '0'; while (u) { tmp[m++] = (char)('0' + u % 10); u /= 10; } if (neg) tmp[m++] = '-';
                       while (m && len < 500) buf[len++] = tmp[--m]; }
                if (r.type == V_STR) { for (int i = 0; i < r.slen && len < 500; i++) buf[len++] = r.sp[i]; }
                else { char tmp[16]; int neg = r.num < 0, m = 0; uint32_t u = neg ? (uint32_t)(-(int64_t)r.num) : (uint32_t)r.num;
                       if (!u) tmp[m++] = '0'; while (u) { tmp[m++] = (char)('0' + u % 10); u /= 10; } if (neg) tmp[m++] = '-';
                       while (m && len < 500) buf[len++] = tmp[--m]; }
                buf[len] = 0;
                return v_str(a_strn(buf, len), len);
            }
            if (l.type == V_NUM && r.type == V_NUM) return v_num(l.num + r.num);
            rerr("+ needs numbers/strings"); return VAL_UNDEF;
        }
        case OP_SUB: if (l.type == V_NUM && r.type == V_NUM) return v_num(l.num - r.num); break;
        case OP_MUL: if (l.type == V_NUM && r.type == V_NUM) return v_num(l.num * r.num); break;
        case OP_DIV: if (l.type == V_NUM && r.type == V_NUM) {
            if (!r.num) { rerr("divide by zero"); return VAL_UNDEF; }
            return v_num(l.num / r.num);
        } break;
        case OP_MOD: if (l.type == V_NUM && r.type == V_NUM) {
            if (!r.num) { rerr("modulo zero"); return VAL_UNDEF; }
            return v_num(l.num % r.num);
        } break;
        case OP_EQ: {
            if (l.type != r.type) return v_bool(0);
            if (l.type == V_NUM)  return v_bool(l.num == r.num);
            if (l.type == V_STR)  return v_bool(l.slen == r.slen && scmp(l.sp, r.sp) == 0);
            if (l.type == V_BOOL) return v_bool(l.num == r.num);
            if (l.type == V_OBJ || l.type == V_ARR) return v_bool(l.op == r.op);
            return v_bool(1);                        /* null == null etc. */
        }
        case OP_NE: {
            JSVal l2 = eval_expr(n->a), r2 = eval_expr(n->b);
            int eq;
            if (l2.type != r2.type) eq = 0;
            else if (l2.type == V_NUM) eq = (l2.num == r2.num);
            else if (l2.type == V_STR) eq = (l2.slen == r2.slen && scmp(l2.sp, r2.sp) == 0);
            else if (l2.type == V_BOOL) eq = (l2.num == r2.num);
            else if (l2.type == V_OBJ || l2.type == V_ARR) eq = (l2.op == r2.op);
            else eq = 1;
            return v_bool(!eq);
        }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            if (l.type == V_NUM && r.type == V_NUM) {
                switch (n->num) {
                case OP_LT: return v_bool(l.num < r.num);
                case OP_LE: return v_bool(l.num <= r.num);
                case OP_GT: return v_bool(l.num > r.num);
                case OP_GE: return v_bool(l.num >= r.num);
                }
            }
            if (l.type == V_STR && r.type == V_STR) {
                int c = scmp(l.sp, r.sp);
                switch (n->num) {
                case OP_LT: return v_bool(c < 0);
                case OP_LE: return v_bool(c <= 0);
                case OP_GT: return v_bool(c > 0);
                case OP_GE: return v_bool(c >= 0);
                }
            }
            rerr("comparison needs numbers/strings"); return VAL_UNDEF;
        }
        }
        rerr("bad binary op"); return VAL_UNDEF;
    }
    case E_UN: {
        JSVal v = eval_expr(n->a);
        if (n->num == '!') return v_bool(!truthy(v));
        if (v.type == V_NUM) return v_num(-v.num);
        if (v.type == V_STR) {                  /* numeric string -> num */
            int iv; if (atoi_n(v.sp, &iv) > 0) return v_num(-iv);
        }
        rerr("- needs a number"); return VAL_UNDEF;
    }
    case E_PRE: {
        JSVal v = eval_expr(n->a);
        if (v.type != V_NUM) { rerr("++/-- needs a number"); return VAL_UNDEF; }
        JSVal nv = v_num(v.num + n->num);
        if (assign_target(n->a, nv) != 0) return VAL_UNDEF;
        return nv;
    }
    case E_POST: {
        JSVal v = eval_expr(n->a);
        if (v.type != V_NUM) { rerr("++/-- needs a number"); return VAL_UNDEF; }
        JSVal nv = v_num(v.num + n->num);
        if (assign_target(n->a, nv) != 0) return VAL_UNDEF;
        return v;
    }
    case E_MEMBER: {
        JSVal base = eval_expr(n->a);
        if (n->flag == 1) {                     /* base[idx] */
            JSVal iv = eval_expr(n->b);
            if (base.type == V_ARR && iv.type == V_NUM && arr_index((JSArr *)base.op, iv.num))
                return ((JSArr *)base.op)->v[iv.num];
            if (base.type == V_STR && iv.type == V_NUM && iv.num >= 0 && iv.num < base.slen) {
                char c = base.sp[iv.num];
                return v_str(a_strn(&c, 1), 1);
            }
            return VAL_UNDEF;
        }
        const char *key = n->b ? n->b->sp : 0;
        if (base.type == V_OBJ) {
            VarE *e = obj_get((JSObj *)base.op, key);
            return e ? e->v : VAL_UNDEF;
        }
        if (base.type == V_ARR) {
            if (scmp(key, "length") == 0) return v_num(((JSArr *)base.op)->n);
            return VAL_UNDEF;
        }
        if (base.type == V_STR) {
            if (scmp(key, "length") == 0) return v_num(base.slen);
            return VAL_UNDEF;
        }
        if (base.type == V_FUNC) return VAL_UNDEF;
        return VAL_UNDEF;
    }
    case E_ARR: {
        JSArr *a = arr_new();
        if (!a) { rerr("out of memory"); return VAL_UNDEF; }
        for (Node *e = n->a; e; e = e->next) arr_push(a, eval_expr(e));
        JSVal v = { V_ARR, 0, 0, 0, 0, a };
        return v;
    }
    case E_OBJ: {
        JSObj *o = obj_new();
        if (!o) { rerr("out of memory"); return VAL_UNDEF; }
        for (Node *k = n->a; k; k = k->next) {
            if (!k->sp) continue;
            obj_set(o, k->sp, eval_expr(k->a));
        }
        JSVal v = { V_OBJ, 0, 0, 0, 0, o };
        return v;
    }
    case E_CALL: {
        /* method calls on arrays/objects/strings get special treatment */
        if (n->a && n->a->op == E_MEMBER && n->a->flag == 0) {
            JSVal base = eval_expr(n->a->a);
            const char *key = n->a->b ? n->a->b->sp : 0;
            if (base.type == V_ARR) {
                if (scmp(key, "push") == 0) {
                    JSVal a0 = n->b ? eval_expr(n->b) : VAL_UNDEF;
                    arr_push((JSArr *)base.op, a0);
                    return v_num(((JSArr *)base.op)->n);
                }
                if (scmp(key, "pop") == 0) {
                    JSArr *a = (JSArr *)base.op;
                    if (a->n == 0) return VAL_UNDEF;
                    return a->v[--a->n];
                }
            }
            if (base.type == V_OBJ) {
                VarE *e = obj_get((JSObj *)base.op, key);
                if (e && e->v.type == V_FUNC) {
                    /* gather args */
                    JSVal args[16]; int argc = 0;
                    for (Node *an = n->b; an && argc < 16; an = an->next) args[argc++] = eval_expr(an);
                    JSFunc *f = (JSFunc *)e->v.op;
                    if (f->bid >= 0) {
                        JSVal tmp[17]; tmp[0] = base;
                        for (int i = 0; i < argc; i++) tmp[i + 1] = args[i];
                        return call_builtin(CE_API(), f->bid, tmp, argc + 1);
                    }
                    return call_user(CE_API(), f, args, argc, base);
                }
            }
        }
        /* generic call */
        JSVal callee = eval_expr(n->a);
        JSVal args[16]; int argc = 0;
        for (Node *an = n->b; an && argc < 16; an = an->next) args[argc++] = eval_expr(an);
        if (callee.type != V_FUNC) {
#ifdef NODE_DEBUG
            {
                catos_api_t *api = CE_API();
                api->write(" [calln@"); api->print_int((int)(uintptr_t)n);
                api->write(" callee@"); api->print_int((int)(uintptr_t)n->a);
                api->write(" callee-op="); api->print_int(n->a ? n->a->op : -1);
                if (n->a && n->a->op == E_ID) { api->write(" id="); api->write(n->a->sp); }
                api->write("]");
            }
#endif
            g_errnum = callee.type; rerr("not a function"); return VAL_UNDEF;
        }
        JSFunc *f = (JSFunc *)callee.op;
        if (f->bid >= 0) return call_builtin(CE_API(), f->bid, args, argc);
        return call_user(CE_API(), f, args, argc, VAL_UNDEF);
    }
    }
    return VAL_UNDEF;
}

/* call a user function; thisv may be VAL_UNDEF (plain call) */
static JSVal call_user(catos_api_t *api, JSFunc *f, JSVal *args, int argc, JSVal thisv) {
    if (g_depth >= 100) { rerr("call stack too deep"); return VAL_UNDEF; }
    g_depth++;
    Scope *s = scope_new(f->scope);
    if (!s) { rerr("out of memory"); g_depth--; return VAL_UNDEF; }
    int i = 0;
    if (thisv.type != V_UNDEF) {
        VarE *e = scope_declare(s, "this");
        if (e) e->v = thisv;
    }
    for (Node *p = f->params; p; p = p->next, i++) {
        VarE *e = scope_declare(s, p->sp);
        if (e) e->v = (i < argc) ? args[i] : VAL_UNDEF;
    }
    Scope *old = g_scope;
    g_scope = s;
    int oldflow = g_flow;
    g_flow = 0;
    if (f->body) exec_list(f->body);
    JSVal rv = (g_flow == 3) ? g_ret : VAL_UNDEF;
    g_flow = oldflow;
    g_scope = old;
    g_depth--;
    return rv;
}

/* print / console.log / typeof */
static void out_value(catos_api_t *api, JSVal v, int depth) {
    if (depth > 4) { out_pushs("..."); return; }
    switch (v.type) {
    case V_UNDEF: out_pushs("undefined"); break;
    case V_NULL:  out_pushs("null"); break;
    case V_BOOL:  out_pushs(v.num ? "true" : "false"); break;
    case V_NUM:   out_pushn(v.num); break;
    case V_STR:   for (int i = 0; i < v.slen; i++) out_pushc(v.sp[i]); break;
    case V_FUNC:  out_pushs("[function "); out_pushs(((JSFunc *)v.op)->name ? ((JSFunc *)v.op)->name : ""); out_pushc(']'); break;
    case V_ARR: {
        JSArr *a = (JSArr *)v.op;
        out_pushc('[');
        for (int i = 0; i < a->n; i++) {
            if (i) out_pushs(", ");
            out_value(api, a->v[i], depth + 1);
        }
        out_pushc(']');
        break;
    }
    case V_OBJ: {
        JSObj *o = (JSObj *)v.op;
        out_pushc('{');
        for (int i = 0; i < o->n; i++) {
            if (i) out_pushs(", ");
            out_pushs(o->v[i].name);
            out_pushc(':');
            out_value(api, o->v[i].v, depth + 1);
        }
        out_pushc('}');
        break;
    }
    }
}

static JSVal call_builtin(catos_api_t *api, int bid, JSVal *args, int argc) {
    if (bid == 0) {                          /* print */
        out_reset();
        for (int i = 0; i < argc; i++) {
            if (i) out_pushc(' ');
            out_value(api, args[i], 0);
        }
        out_flush(api);
        return VAL_UNDEF;
    }
    if (bid == 2) {                          /* console.log (arg0 = console) */
        out_reset();
        for (int i = 1; i < argc; i++) {
            if (i > 1) out_pushc(' ');
            out_value(api, args[i], 0);
        }
        out_flush(api);
        return VAL_UNDEF;
    }
    if (bid == 1) {                             /* typeof */
        const char *s;
        switch (args[0].type) {
        case V_NUM:  s = "number"; break;
        case V_STR:  s = "string"; break;
        case V_BOOL: s = "boolean"; break;
        case V_FUNC: s = "function"; break;
        case V_NULL: case V_OBJ: case V_ARR: s = "object"; break;
        default:     s = "undefined"; break;
        }
        return v_str(s, slen(s));
    }
    rerr("unknown builtin");
    return VAL_UNDEF;
}

/* ====================================================================
 * statement execution
 * ================================================================== */
static void exec_list(Node *n) {
    for (; n && !g_err && g_flow == 0; n = n->next) exec_stmt(n);
}

static void exec_stmt(Node *n) {
    if (!n || g_err || g_flow) return;
    switch (n->op) {
    case S_EMPTY: break;
    case S_BLOCK: exec_list(n->body); break;
    case S_EXPR:  eval_expr(n->a); break;
    case S_VAR:
        for (Node *d = n->a; d; d = d->next) {
            JSVal v = d->flag ? eval_expr(d->a) : VAL_UNDEF;
            VarE *e = scope_declare(g_scope, d->sp);
            if (e) e->v = v;
        }
        break;
    case S_FUNC: {
        JSFunc *f = func_new(n->sp, n->a, n->body, g_scope);
        if (!f) { rerr("out of memory"); return; }
        JSVal v = { V_FUNC, 0, 0, n->sp, 0, f };
        VarE *e = scope_declare(g_scope, n->sp);
        if (e) e->v = v;
        break;
    }
    case S_IF:
        if (truthy(eval_expr(n->a))) exec_stmt(n->b);
        else if (n->c) exec_stmt(n->c);
        break;
    case S_WHILE:
        while (!g_err && g_flow == 0 && truthy(eval_expr(n->a))) {
            exec_stmt(n->body);
            if (g_flow == 1) g_flow = 0;        /* break */
            if (g_flow == 2) g_flow = 0;        /* continue */
        }
        break;
    case S_FOR: {
        if (n->a) {
            if (n->a->op == S_VAR) exec_stmt(n->a);
            else eval_expr(n->a);
        }
        for (;;) {
            if (g_err || g_flow) break;
            if (n->b && !truthy(eval_expr(n->b))) break;
            exec_stmt(n->body);
            if (g_flow == 1) { g_flow = 0; break; }
            if (g_flow == 2) g_flow = 0;
            if (g_flow == 3) break;
            if (n->c) eval_expr(n->c);
        }
        break;
    }
    case S_BREAK:    g_flow = 1; break;
    case S_CONT:     g_flow = 2; break;
    case S_RETURN:   g_ret = n->a ? eval_expr(n->a) : VAL_UNDEF; g_flow = 3; break;
    default: rerr("bad statement"); break;
    }
}

/* ====================================================================
 * built-in demo script
 * ================================================================== */
static const char DEMO_JS[] =
    "/* CatOS node-subset demo: vars/functions/loops/arrays/objects */\n"
    "var x = 5;\n"
    "var y = x * 2 + 1;\n"
    "var name = \"catos\";\n"
    "var ok = true;\n"
    "function fib(n) {\n"
    "    if (n < 2) return n;\n"
    "    return fib(n - 1) + fib(n - 2);\n"
    "}\n"
    "function greet(who, ex) {\n"
    "    var msg = \"hello \" + who;\n"
    "    if (ex == true) msg = msg + \"!\";\n"
    "    return msg;\n"
    "}\n"
    "var arr = [1, 2, 3];\n"
    "arr.push(4);\n"
    "arr[0] = 10;\n"
    "var total = 0;\n"
    "for (var i = 0; i < arr.length; i = i + 1) total = total + arr[i];\n"
    "var s = 0;\n"
    "var j = 0;\n"
    "while (j < 4) {\n"
    "    if (j == 2) { j = j + 1; continue; }\n"
    "    s = s + j;\n"
    "    j = j + 1;\n"
    "}\n"
    "var obj = { title: \"node-subset\", ver: 1, tags: [\"js\", \"catos\"], meta: { arch: \"i386\" } };\n"
    "obj.level = 3;\n"
    "print(\"x=\", x, \" y=\", y, \" name=\", name, \" ok=\", ok);\n"
    "print(\"fib(10)=\", fib(10));\n"
    "print(\"greet=\", greet(\"world\", true));\n"
    "print(\"arr=\", arr, \" len=\", arr.length);\n"
    "print(\"total=\", total);\n"
    "print(\"while s=\", s);\n"
    "print(\"obj.title=\", obj.title, \" ver=\", obj.ver, \" level=\", obj.level);\n"
    "print(\"obj.tags[1]=\", obj.tags[1], \" meta.arch=\", obj.meta.arch);\n"
    "print(\"typeof x=\", typeof(x), \" typeof arr=\", typeof(arr), \" typeof name=\", typeof(name));\n"
    "print(\"null=\", null, \" undefined=\", undefined);\n"
    "print(\"done\");\n";

/* ====================================================================
 * entry
 * ================================================================== */
static char g_srcbuf[8192];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    int srclen = 0;

    api->write("CatOS node subset v1.0\n");
    api->write("----------------------\n");

    /* ---- pick the script source ---- */
    const char *path = 0;
    if (argc >= 2 && argv[1] && argv[1][0]) path = argv[1];
    else if (api->file_exists("/BIN/DEMO.JS")) path = "/BIN/DEMO.JS";
    if (path) {
        int got = api->file_read(path, g_srcbuf, (int)sizeof(g_srcbuf) - 1, 0);
        if (got < 0) {
            api->write("node: cannot read script: ");
            api->write(path);
            api->write("\n");
            return 1;
        }
        g_srcbuf[got] = 0;
        srclen = got;
        api->write("script: ");
        api->write(path);
        api->write(" (");
        api->print_int(got);
        api->write(" bytes)\n");
    } else {
        for (int i = 0; DEMO_JS[i] && i < (int)sizeof(g_srcbuf) - 1; i++)
            g_srcbuf[i] = DEMO_JS[i];
        srclen = slen(DEMO_JS);
        g_srcbuf[srclen] = 0;
        api->write("script: built-in demo\n");
    }

    /* ---- global scope with builtins ---- */
    g_apos = 0; g_oom = 0; g_err = 0; g_flow = 0; g_depth = 0;
    g_scope = 0;
    g_globals = scope_new(0);
    if (!g_globals) { api->write("node: out of memory\n"); return 1; }
    {
        JSVal v = { V_FUNC, 0, 0, "print", 0, bfunc_new("print", 0) };
        scope_declare(g_globals, "print")->v = v;
        v = (JSVal){ V_FUNC, 0, 0, "typeof", 0, bfunc_new("typeof", 1) };
        scope_declare(g_globals, "typeof")->v = v;
        JSObj *con = obj_new();
        if (con) {
            JSVal logv = { V_FUNC, 0, 0, "log", 0, bfunc_new("log", 2) };
            obj_set(con, "log", logv);
            v = (JSVal){ V_OBJ, 0, 0, 0, 0, con };
            scope_declare(g_globals, "console")->v = v;
        }
    }
    g_scope = g_globals;

    /* ---- lex + parse ---- */
    lex_run(g_srcbuf, srclen);
    g_ti = 0; g_perr = 0;
    Node *prog = p_stmt_list();
#ifdef NODE_DEBUG
    /* AST dump (shallow: statement ops + first child ops) */
    {
        int ns = 0;
        for (Node *s = prog; s; s = s->next) {
            api->write("st"); api->print_int(ns++);
            api->write(" op="); api->print_int(s->op);
            if (s->op == S_VAR && s->a) { api->write(" decl-op="); api->print_int(s->a->op); api->write(" name="); api->write(s->a->sp); if (s->a->a) { api->write(" init-op="); api->print_int(s->a->a->op); } }
            if (s->op == S_FUNC) { api->write(" fn="); api->write(s->sp); api->write(" param-op="); api->print_int(s->a ? s->a->op : -1); }
            if (s->op == S_EXPR && s->a) { api->write(" expr-op="); api->print_int(s->a->op); if (s->a->op == E_CALL) { api->write(" callee-op="); api->print_int(s->a->a ? s->a->a->op : -1); } }
            api->write("\n");
        }
    }
#endif
#ifdef NODE_DEBUG
    {
        char dbg[64];
        for (int i = 0; i < g_tokn; i++) {
            Tok *t = &g_tok[i];
            api->putc(t->type == T_NUM ? 'N' : t->type == T_ID ? 'I' : t->type == T_STR ? 'S' : t->type == T_EOF ? 'E' : 'O');
            if (t->type == T_ID) { api->write(":"); for (int k = 0; k < t->len; k++) api->putc(t->sp[k]); }
            if (t->type == T_OP) { api->putc('['); api->putc((char)t->num); api->putc(']'); }
            if (t->type == T_NUM) { api->write(":"); api->print_int(t->num); }
            api->write("("); api->print_int(i); api->putc(')');
            api->putc(' ');
            if (i % 8 == 7) api->putc('\n');
        }
        api->putc('\n');
        api->write("oom="); api->print_int(g_oom);
        api->putc('\n');
        (void)dbg;
    }
#endif
    if (g_oom) { api->write("node: out of memory (lex)\n"); return 1; }
    if (g_ti < g_tokn - 1 || g_perr || g_tok[g_ti].type != T_EOF) {
        api->write("node: syntax error");
#ifdef NODE_DEBUG
        {
            Tok *t = &g_tok[g_ti];
            api->write(" at tok#");
            api->print_int(g_ti);
            api->write(" type=");
            api->print_int(t->type);
            if (t->type == T_ID) { api->write(" id="); for (int k = 0; k < t->len && k < 24; k++) api->putc(t->sp[k]); }
            if (t->type == T_OP) { api->write(" op="); api->putc((char)t->num); api->write(" len="); api->print_int(t->len); }
        }
#endif
        api->write("\n");
        return 1;
    }

    /* ---- run ---- */
    exec_list(prog);
#ifdef NODE_DEBUG
    {
        api->write("globals:");
        for (Scope *s = g_globals; s; s = s->parent)
            for (int i = 0; i < s->n; i++) {
                api->write(" "); api->write(s->v[i].name); api->write("=");
                api->print_int(s->v[i].v.type);
            }
        api->write("\n");
    }
#endif
    if (g_err) {
        if (g_oom) api->write("node: runtime out of memory\n");
        else if (g_errmsg) {
#ifdef NODE_DEBUG
            api->write("node: runtime error: ");
            api->write(g_errmsg);
            api->write(" num="); api->print_int(g_errnum);
            api->write("\n");
#else
            api->write("node: runtime error: "); api->write(g_errmsg); api->write("\n");
#endif
        }
        else api->write("node: runtime error\n");
        return 1;
    }
    api->write("node: exit ok\n");
    return 0;
}
