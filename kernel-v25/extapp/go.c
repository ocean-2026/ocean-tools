/*
 * go.ce -- CatOS "go" subset: a tiny Go-syntax interpreter (CE user app).
 *
 * A Go-syntax script interpreter built on the same CE runtime contract as
 * node.c.  Supported subset:
 *
 *   comments:    // line   and   /* block * /
 *   literals:    123  -4  "str\n"  true  false  'A'(char -> int)
 *   declarations: var x = expr      (short form also: x := expr)
 *   assignment:  x = expr           a[i] = expr
 *   arithmetic:  +  -  *  /  %      (32-bit truncating, like C;
 *                                   + on strings = concatenation)
 *   compare:      == !=  <  <=  >  >=
 *   logic:        &&  ||  !
 *   control:      if cond { } else { }
 *                 for init; cond; post { }
 *                 for cond { }        for { }
 *                 break  continue  return expr
 *   functions:    func name(p1, p2) { ... return expr }   (recursion ok)
 *   arrays:       [1,2,3]  (mixed Values allowed) ; a[i] read/write ;
 *                 len(a)
 *   builtins:     print(...)  (space separated, newline flushed)
 *                 len(s | a)
 *                 readfile(path) -> string  ("" on error)
 *                 argv(n) -> string         (go.ce command args)
 *                 atoi(s) -> int
 *                 chr(n)  -> string        (one byte)
 *                 sub(s, start, n) -> string
 *                 s[i]    -> int           (byte value at i)
 *
 * Integers only (the CE build has no FPU; -mno-80387).
 *
 * Script selection (first match wins):
 *   1. argv[1]  -> run /bin/go.ce /PATH/TO/SCRIPT.GO   (argv[2] optional,
 *                  exposed to the script via argv(n) for self-hosting)
 *   2. /BIN/DEMO.GO on the volume -> executed
 *   3. otherwise the embedded demo runs.
 *
 * Build: sh extapp/build_ce.sh go.c   -> extapp/bin/go.ce
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* ====================================================================
 * tiny freestanding helpers (no libc)
 * ================================================================== */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void sncpy(char *d, const char *s, int n) {
    while (n-- > 0 && *s) *d++ = *s++;
    *d = 0;
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int scmpn(const char *a, const char *b, int nb) {
    int i = 0;
    while (i < nb && a[i] == b[i] && a[i]) i++;
    if (i < nb) return 1;
    return a[i] != 0;   /* lengths differ */
}
static int is_idc(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* ====================================================================
 * arena memory: one big block taken from the kernel heap (api->alloc),
 * bump-allocated.  The interpreter runs one script and exits, so freeing
 * is unnecessary.  Falls back to a static block if the heap call fails.
 * ================================================================== */
#define ARENA_SZ (192u * 1024u)
static uint8_t g_arena_static[64u * 1024u];   /* fallback only */
static uint8_t *g_arena;
static uint32_t g_apos;
static int g_oom;

static void *a_alloc(uint32_t n) {
    n = (n + 7u) & ~7u;
    if (g_apos + n > ARENA_SZ) { g_oom = 1; return 0; }
    void *p = g_arena + g_apos;
    g_apos += n;
    return p;
}
static char *a_str(const char *s) {
    int n = slen(s) + 1;
    char *p = (char *)a_alloc((uint32_t)n);
    if (p) scpy(p, s);
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
enum { V_UNDEF, V_NUM, V_STR, V_BOOL, V_FUNC, V_ARR };
typedef struct GVal {
    uint8_t  type;
    int32_t  num;          /* number / bool / char byte */
    const char *sp;       /* string data */
    int32_t  slen;        /* string length */
    void    *op;          /* array / function pointer */
} GVal;

typedef struct VarE { const char *name; GVal v; } VarE;
typedef struct Scope { VarE *v; int n, cap; struct Scope *parent; } Scope;
typedef struct GArr { GVal *v; int n, cap; } GArr;
struct Node;
typedef struct GFunc {
    const char *name;
    struct Node *params, *body;
    Scope *scope;
    int bid;              /* builtin id, -1 = user function */
} GFunc;

static GVal v_num(int32_t n)   { GVal x = { V_NUM, n, 0, 0, 0 }; return x; }
static GVal v_bool(int b)      { GVal x = { V_BOOL, b, 0, 0, 0 }; return x; }
static GVal v_str(const char *p, int n) { GVal x = { V_STR, 0, p, n, 0 }; return x; }

/* ====================================================================
 * AST
 * ================================================================== */
enum {   /* statements */
    S_BLOCK, S_VAR, S_ASSIGN, S_IF, S_FOR, S_BREAK, S_CONT,
    S_RETURN, S_EXPR, S_FUNC, S_EMPTY
};
enum {   /* expressions */
    E_NUM, E_STR, E_ID, E_BOOL, E_UNDEF, E_DECL, E_BIN,
    E_UN, E_CALL, E_INDEX, E_ARRLIT
};
enum {   /* E_BIN op codes */
    OP_ADD='+', OP_SUB='-', OP_MUL='*', OP_DIV='/', OP_MOD='%',
    OP_EQ=1, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_AND, OP_OR
};

typedef struct Node {
    uint8_t op;
    uint8_t flag;         /* S_ASSIGN: 1 short-var := (decl) ; E_INDEX: unused */
    int32_t num;
    const char *sp;
    struct Node *a, *b, *c;
    struct Node *body;
    struct Node *next;
} Node;

static Node *n_new(int op) {
    Node *n = (Node *)a_alloc(sizeof(Node));
    if (n) {
        n->op = (uint8_t)op; n->flag = 0; n->num = 0; n->sp = 0;
        n->a = n->b = n->c = 0; n->body = 0; n->next = 0;
    }
    return n;
}

/* ====================================================================
 * lexer
 * ================================================================== */
enum { T_NUM, T_ID, T_STR, T_OP, T_EOF };
typedef struct Tok {
    uint8_t type;
    int32_t num;
    const char *sp;       /* for T_STR: decoded bytes in arena; for T_ID: raw src */
    int len;
    int line;             /* 1-based source line where this token starts */
} Tok;

#define TOK_MAX 4096
static Tok g_tok[TOK_MAX];
static int g_tokn;

static const char *g_src;
static int g_pos;
static int g_slen;
static int g_line;          /* 1-based source line for error messages */

static void lex_add(int type, const char *sp, int len, int32_t num) {
    if (g_tokn >= TOK_MAX) return;
    Tok *t = &g_tok[g_tokn++];
    t->type = (uint8_t)type; t->sp = sp; t->len = len; t->num = num;
    t->line = g_line;
}

static int lex_skip_ws(void) {
    for (;;) {
        int c = g_src[g_pos];
        if (c == ' ' || c == '\t' || c == '\r') { g_pos++; continue; }
        if (c == '\n') { g_pos++; g_line++; continue; }
        if (c == '/' && g_src[g_pos + 1] == '/') {
            while (g_src[g_pos] && g_src[g_pos] != '\n') g_pos++;
            continue;
        }
        if (c == '/' && g_src[g_pos + 1] == '*') {
            g_pos += 2;
            while (g_src[g_pos] &&
                   !(g_src[g_pos] == '*' && g_src[g_pos + 1] == '/')) {
                if (g_src[g_pos] == '\n') g_line++;
                g_pos++;
            }
            if (g_src[g_pos]) g_pos += 2;
            continue;
        }
        return c;
    }
}

static void lex_string(void) {
    g_pos++;                                   /* opening " */
    char buf[1024]; int n = 0;
    while (g_src[g_pos] && g_src[g_pos] != '"' && n < (int)sizeof(buf) - 1) {
        int c = g_src[g_pos++];
        if (c == '\\' && g_src[g_pos]) {
            int e = g_src[g_pos++];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '0': c = 0; break;
            default: c = e; break;
            }
        }
        buf[n++] = (char)c;
    }
    if (g_src[g_pos] == '"') g_pos++;
    buf[n] = 0;
    lex_add(T_STR, a_strn(buf, n), n, 0);
}

static void lex_char(void) {
    g_pos++;                                   /* opening ' */
    int v = 0;
    if (g_src[g_pos] == '\\' && g_src[g_pos + 1]) {
        g_pos++;
        int e = g_src[g_pos++];
        v = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r'
              : (e == '0') ? 0 : e;
    } else {
        v = (unsigned char)g_src[g_pos++];
    }
    while (g_src[g_pos] && g_src[g_pos] != '\'') g_pos++;
    if (g_src[g_pos] == '\'') g_pos++;
    lex_add(T_NUM, 0, 0, v);
}

static int lex_two(char a, char b, int op1, int op2) {
    if (g_src[g_pos + 1] == b) {
        g_pos += 2;
        lex_add(T_OP, 0, 0, op2);
        return 1;
    }
    g_pos++;
    lex_add(T_OP, 0, 0, op1);
    return 1;
}

static void lex_run(const char *src, int len) {
    g_src = src; g_slen = len; g_pos = 0; g_tokn = 0; g_line = 1;
    for (;;) {
        int c = lex_skip_ws();
        if (!c || g_pos >= g_slen) { lex_add(T_EOF, src + g_pos, 0, 0); break; }
        if (is_digit(c)) {
            int v = 0;
            while (is_digit(g_src[g_pos])) v = v * 10 + (g_src[g_pos++] - '0');
            lex_add(T_NUM, 0, 0, v);
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            const char *sp = src + g_pos;
            while (is_idc(g_src[g_pos])) g_pos++;
            lex_add(T_ID, sp, g_pos - (sp - src), 0);
            continue;
        }
        if (c == '"') { lex_string(); continue; }
        if (c == '\'') { lex_char(); continue; }
        switch (c) {
        case '+': if (g_src[g_pos+1]=='+') { g_pos++; lex_add(T_OP,0,0,';'); }
                  else lex_two('+','=',OP_ADD,';'); break; /* ++ unused; treat ';' */
        case '-': if (g_src[g_pos+1]=='-') { g_pos++; lex_add(T_OP,0,0,';'); }
                  else lex_two('-','=',OP_SUB,';'); break;
        case '*': lex_two('*','=',OP_MUL,';'); break;
        case '/': lex_two('/','=',OP_DIV,';'); break;
        case '%': lex_add(T_OP,0,0,OP_MOD); g_pos++; break;
        case '=':
            if (g_src[g_pos+1] == '=') { g_pos += 2; lex_add(T_OP,0,0,OP_EQ); }
            else if (g_src[g_pos+1] == '>') { g_pos += 2; lex_add(T_OP,0,0,OP_ADD); } /* => ignored */
            else { g_pos++; lex_add(T_OP,0,0,'='); }
            break;
        case '!':
            if (g_src[g_pos+1] == '=') { g_pos += 2; lex_add(T_OP,0,0,OP_NE); }
            else { g_pos++; lex_add(T_OP,0,0,'!'); }
            break;
        case '<': lex_two('<','=',OP_LT,OP_LE); break;
        case '>': lex_two('>','=',OP_GT,OP_GE); break;
        case '&': lex_two('&','&',OP_AND,OP_AND); break;
        case '|': lex_two('|','|',OP_OR,OP_OR); break;
        case '(': lex_add(T_OP,0,0,'('); g_pos++; break;
        case ')': lex_add(T_OP,0,0,')'); g_pos++; break;
        case '{': lex_add(T_OP,0,0,'{'); g_pos++; break;
        case '}': lex_add(T_OP,0,0,'}'); g_pos++; break;
        case '[': lex_add(T_OP,0,0,'['); g_pos++; break;
        case ']': lex_add(T_OP,0,0,']'); g_pos++; break;
        case ',': lex_add(T_OP,0,0,','); g_pos++; break;
        case ';': lex_add(T_OP,0,0,';'); g_pos++; break;
        case ':':
            if (g_src[g_pos+1] == '=') { g_pos += 2; lex_add(T_OP,0,0,':'); } /* short := */
            else { g_pos++; lex_add(T_OP,0,0,':'); }
            break;
        default: g_pos++; break;
        }
    }
}

/* ====================================================================
 * parser
 * ================================================================== */
static int g_ti;                 /* current token index */
static int g_perr;
static int g_errline;

static Tok *cur(void) { return &g_tok[g_ti]; }
static int tok_is_op(int op) {
    Tok *t = cur();
    return t->type == T_OP && t->num == op;
}
static int tok_is_id(const char *w) {
    Tok *t = cur();
    if (t->type != T_ID) return 0;
    int n = slen(w);
    if (t->len != n) return 0;
    for (int i = 0; i < n; i++) if (t->sp[i] != w[i]) return 0;
    return 1;
}
static void advance(void) { if (g_ti < g_tokn - 1) g_ti++; }

static Node *p_block(void);
static Node *p_stmt(void);
static Node *p_expr(void);

static void perr(const char *msg) {
    if (!g_perr) { g_perr = 1; g_errline = cur()->line; }
    (void)msg;
}

/* parse an expression; precedence climbing */
static Node *p_or(void);
static Node *p_and(void);
static Node *p_cmp(void);
static Node *p_add(void);
static Node *p_mul(void);
static Node *p_unary(void);
static Node *p_postfix(void);
static Node *p_primary(void);

static Node *p_or(void) {
    Node *l = p_and();
    while (tok_is_op(OP_OR)) {
        advance();
        Node *n = n_new(E_BIN); n->a = l; n->num = OP_OR; n->b = p_and();
        l = n;
    }
    return l;
}
static Node *p_and(void) {
    Node *l = p_cmp();
    while (tok_is_op(OP_AND)) {
        advance();
        Node *n = n_new(E_BIN); n->a = l; n->num = OP_AND; n->b = p_cmp();
        l = n;
    }
    return l;
}
static Node *p_cmp(void) {
    Node *l = p_add();
    for (;;) {
        int op = cur()->num;
        if (cur()->type == T_OP && (op == OP_EQ || op == OP_NE || op == OP_LT ||
                                    op == OP_LE || op == OP_GT || op == OP_GE)) {
            advance();
            Node *n = n_new(E_BIN); n->a = l; n->num = op; n->b = p_add();
            l = n;
        } else break;
    }
    return l;
}
static Node *p_add(void) {
    Node *l = p_mul();
    while (tok_is_op(OP_ADD) || tok_is_op(OP_SUB)) {
        int op = cur()->num; advance();
        Node *n = n_new(E_BIN); n->a = l; n->num = op; n->b = p_mul();
        l = n;
    }
    return l;
}
static Node *p_mul(void) {
    Node *l = p_unary();
    while (tok_is_op(OP_MUL) || tok_is_op(OP_DIV) || tok_is_op(OP_MOD)) {
        int op = cur()->num; advance();
        Node *n = n_new(E_BIN); n->a = l; n->num = op; n->b = p_unary();
        l = n;
    }
    return l;
}
static Node *p_unary(void) {
    if (tok_is_op('!')) { advance(); Node *n = n_new(E_UN); n->num = '!'; n->a = p_unary(); return n; }
    if (tok_is_op(OP_SUB)) { advance(); Node *n = n_new(E_UN); n->num = OP_SUB; n->a = p_unary(); return n; }
    return p_postfix();
}
static Node *p_postfix(void) {
    Node *e = p_primary();
    for (;;) {
        if (tok_is_op('[')) {
            advance();
            Node *idx = p_expr();
            if (tok_is_op(']')) advance(); else perr("]");
            Node *n = n_new(E_INDEX); n->a = e; n->b = idx;
            e = n;
        } else if (tok_is_op('(') && e->op == E_ID) {
            int opline = cur()->line;
            advance();
            Node *args = 0, **tail = &args;
            if (!tok_is_op(')')) {
                *tail = p_expr(); tail = &(*tail)->next;
                while (tok_is_op(',')) { advance(); *tail = p_expr(); tail = &(*tail)->next; }
            }
            if (tok_is_op(')')) advance(); else { perr(")"); g_errline = opline; }
            Node *n = n_new(E_CALL); n->a = e; n->b = args;
            e = n;
        } else break;
    }
    return e;
}
static Node *p_primary(void) {
    Tok *t = cur();
    if (t->type == T_NUM) { advance(); Node *n = n_new(E_NUM); n->num = t->num; return n; }
    if (t->type == T_STR) { advance(); Node *n = n_new(E_STR); n->sp = t->sp; n->num = t->len; return n; }
    if (t->type == T_ID) {
        if (tok_is_id("true"))  { advance(); Node *n = n_new(E_BOOL); n->num = 1; return n; }
        if (tok_is_id("false")) { advance(); Node *n = n_new(E_BOOL); n->num = 0; return n; }
        { const char *id = a_strn(t->sp, t->len); advance();
          Node *n = n_new(E_ID); n->sp = id; return n; }
    }
    if (tok_is_op('(')) {
        int opline = cur()->line;
        advance();
        Node *e = p_expr();
        if (tok_is_op(')')) advance(); else { perr(")"); g_errline = opline; }
        return e;
    }
    if (tok_is_op('[')) {           /* array literal */
        advance();
        Node *args = 0, **tail = &args;
        if (!tok_is_op(']')) {
            *tail = p_expr(); tail = &(*tail)->next;
            while (tok_is_op(',')) { advance(); *tail = p_expr(); tail = &(*tail)->next; }
        }
        if (tok_is_op(']')) advance(); else perr("]");
        Node *n = n_new(E_ARRLIT); n->a = args;
        return n;
    }
    perr("primary");
    advance();
    return n_new(E_UNDEF);
}
static Node *p_expr(void) { return p_or(); }

/* -------- statement list -------- */
static Node *p_stmt_list(void) {
    Node *list = 0, **tail = &list;
    while (!tok_is_op('}') && !tok_is_op(T_EOF) && cur()->type != T_EOF) {
        Node *s = p_stmt();
        if (g_perr) break;
        if (s) { *tail = s; tail = &s->next; }
        while (tok_is_op(';')) advance();
    }
    return list;
}
static Node *p_block(void) {
    if (!tok_is_op('{')) { perr("{"); return 0; }
    advance();
    Node *b = n_new(S_BLOCK);
    b->body = p_stmt_list();
    if (tok_is_op('}')) advance(); else perr("}");
    return b;
}

static Node *p_stmt(void) {
    Tok *t = cur();

    if (t->type == T_ID && tok_is_id("var")) {
        advance();
        Node *v = n_new(S_VAR);
        Node *decls = 0, **tail = &decls;
        for (;;) {
            if (cur()->type != T_ID) { perr("var name"); break; }
            const char *name = cur()->sp; int nlen = cur()->len;
            advance();
            /* optional type name: skip until = or , or ; or \n-block */
            while (cur()->type == T_ID && !tok_is_op('=') && !tok_is_op(':') &&
                   !tok_is_op(',') && !tok_is_op(';')) advance();
            Node *d = n_new(E_DECL);
            d->sp = a_strn(name, nlen);
            if (tok_is_op('=') || tok_is_op(':')) {
                advance();
                d->a = p_expr();
            }
            *tail = d; tail = &d->next;
            if (tok_is_op(',')) { advance(); continue; }
            break;
        }
        v->a = decls;
        return v;
    }

    if (t->type == T_ID && tok_is_id("func")) {
        advance();
        if (cur()->type != T_ID) { perr("func name"); return n_new(S_EMPTY); }
        const char *name = a_strn(cur()->sp, cur()->len); advance();
        Node *f = n_new(S_FUNC); f->sp = name;
        if (tok_is_op('(')) advance(); else perr("(");
        Node *params = 0, **tail = &params;
        if (!tok_is_op(')')) {
            if (cur()->type == T_ID) {
                const char *pn = a_strn(cur()->sp, cur()->len); advance();
                /* skip type */
                while (cur()->type == T_ID && !tok_is_op(',') && !tok_is_op(')')) advance();
                *tail = n_new(E_ID); (*tail)->sp = pn; tail = &(*tail)->next;
                while (tok_is_op(',')) {
                    advance();
                    if (cur()->type == T_ID) {
                        const char *qn = a_strn(cur()->sp, cur()->len); advance();
                        while (cur()->type == T_ID && !tok_is_op(',') && !tok_is_op(')')) advance();
                        *tail = n_new(E_ID); (*tail)->sp = qn; tail = &(*tail)->next;
                    }
                }
            }
        }
        if (tok_is_op(')')) advance(); else perr(")");
        /* optional return type: skip identifiers until { */
        while (cur()->type == T_ID && !tok_is_op('{')) advance();
        f->a = params;
        f->body = p_block();
        return f;
    }

    if (t->type == T_ID && tok_is_id("if")) {
        advance();
        Node *n = n_new(S_IF);
        n->a = p_expr();
        n->b = p_block();
        if (tok_is_id("else")) {
            advance();
            if (tok_is_id("if")) { n->c = p_stmt(); }
            else { n->c = p_block(); }
        }
        return n;
    }

    if (t->type == T_ID && tok_is_id("for")) {
        advance();
        Node *n = n_new(S_FOR);
        /* three-clause:  init; cond; post    (detect ';' ) */
        int has_semi = 0;
        /* peek for init clause */
        if (!tok_is_op('{')) {
            int j = g_ti, found_semi = 0;
            while (j < g_tokn && g_tok[j].type != T_EOF) {
                if (g_tok[j].type == T_OP && g_tok[j].num == '{') break;
                if (g_tok[j].type == T_OP && g_tok[j].num == ';') { found_semi = 1; break; }
                j++;
            }
            has_semi = found_semi;
        }
        if (has_semi) {
            /* init clause */
            if (tok_is_op(';')) { advance(); n->a = 0; }
            else {
                n->a = p_stmt();
                if (tok_is_op(';')) advance(); else perr(";");
            }
            /* cond clause */
            if (tok_is_op(';')) { n->b = 0; advance(); }
            else { n->b = p_expr(); if (tok_is_op(';')) advance(); else perr(";"); }
            /* post clause */
            if (tok_is_op('{')) { n->c = 0; }
            else {
                n->c = p_stmt();
            }
        } else if (!tok_is_op('{')) {
            /* single condition: while-style */
            n->a = 0; n->c = 0;
            n->b = p_expr();
        } else {
            n->a = 0; n->b = 0; n->c = 0;
        }
        n->body = p_block();
        return n;
    }

    if (t->type == T_ID && tok_is_id("return")) {
        advance();
        Node *n = n_new(S_RETURN);
        if (!tok_is_op(';') && !tok_is_op('}') && cur()->type != T_EOF) n->a = p_expr();
        return n;
    }
    if (t->type == T_ID && tok_is_id("break"))  { advance(); return n_new(S_BREAK); }
    if (t->type == T_ID && tok_is_id("continue")){ advance(); return n_new(S_CONT); }
    if (tok_is_op(';')) { advance(); return n_new(S_EMPTY); }

    /* expression or assignment:  lvalue = expr  |  lvalue := expr */
    {
        Node *e = p_expr();
        if (tok_is_op('=') || tok_is_op(':')) {
            int shortv = tok_is_op(':');
            advance();
            Node *r = p_expr();
            Node *n = n_new(S_ASSIGN);
            n->a = e; n->b = r; n->flag = (uint8_t)(shortv ? 1 : 0);
            return n;
        }
        Node *n = n_new(S_EXPR); n->a = e;
        return n;
    }
}

/* ====================================================================
 * scope
 * ================================================================== */
static Scope *g_scope;
static Scope *g_globals;

static Scope *scope_new(Scope *parent) {
    Scope *s = (Scope *)a_alloc(sizeof(Scope));
    if (!s) return 0;
    s->n = 0; s->cap = 8; s->parent = parent;
    s->v = (VarE *)a_alloc(sizeof(VarE) * 8u);
    return s;
}
static VarE *scope_find(Scope *s, const char *name) {
    for (; s; s = s->parent)
        for (int i = 0; i < s->n; i++)
            if (scmp(s->v[i].name, name) == 0) return &s->v[i];
    return 0;
}
static VarE *scope_declare(Scope *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (scmp(s->v[i].name, name) == 0) return &s->v[i];
    if (s->n >= s->cap) {
        int ncap = s->cap * 2;
        VarE *nv = (VarE *)a_alloc(sizeof(VarE) * (uint32_t)ncap);
        if (!nv) return 0;
        for (int i = 0; i < s->n; i++) nv[i] = s->v[i];
        s->v = nv; s->cap = ncap;
    }
    s->v[s->n].name = name;
    s->v[s->n].v.type = V_UNDEF;
    return &s->v[s->n++];
}

/* ====================================================================
 * arrays
 * ================================================================== */
static GArr *arr_new(void) {
    GArr *a = (GArr *)a_alloc(sizeof(GArr));
    if (!a) return 0;
    a->n = 0; a->cap = 4;
    a->v = (GVal *)a_alloc(sizeof(GVal) * 4u);
    return a;
}
static void arr_push(GArr *a, GVal v) {
    if (a->n >= a->cap) {
        int ncap = a->cap * 2;
        GVal *nv = (GVal *)a_alloc(sizeof(GVal) * (uint32_t)ncap);
        if (!nv) return;
        for (int i = 0; i < a->n; i++) nv[i] = a->v[i];
        a->v = nv; a->cap = ncap;
    }
    a->v[a->n++] = v;
}
static GFunc *func_new(const char *name, Node *params, Node *body, Scope *scope) {
    GFunc *f = (GFunc *)a_alloc(sizeof(GFunc));
    if (!f) return 0;
    f->name = name; f->params = params; f->body = body; f->scope = scope; f->bid = -1;
    return f;
}
static GFunc *bfunc_new(const char *name, int bid) {
    GFunc *f = (GFunc *)a_alloc(sizeof(GFunc));
    if (!f) return 0;
    f->name = name; f->params = 0; f->body = 0; f->scope = 0; f->bid = bid;
    return f;
}

/* ====================================================================
 * output (each print() flushes one line)
 * ================================================================== */
static char g_out[256];
static int g_oln;
static void out_reset(void) { g_oln = 0; g_out[0] = 0; }
static void out_pushc(char c) { if (g_oln < (int)sizeof(g_out) - 1) g_out[g_oln++] = c; g_out[g_oln] = 0; }
static void out_pushs(const char *s) { while (*s) out_pushc(*s++); }
static void out_pushn(int32_t v) {
    char tmp[16]; int neg = v < 0, n = 0;
    uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) tmp[n++] = '-';
    while (n) out_pushc(tmp[--n]);
}
static void out_flush(catos_api_t *api) { out_pushc('\n'); api->write(g_out); out_reset(); }

/* ====================================================================
 * interpreter state
 * ================================================================== */
static int g_err;
static const char *g_errmsg;
static int g_flow;            /* 0 normal, 1 break, 2 continue, 3 return */
static GVal g_ret;

static void rerr(const char *msg) { if (!g_err) { g_err = 1; g_errmsg = msg; } }

static GVal eval_expr(Node *n);
static void exec_stmt(Node *n);
static void exec_list(Node *n);
static GVal call_builtin(catos_api_t *api, int bid, GVal *args, int argc);
static GVal call_user(catos_api_t *api, GFunc *f, GVal *args, int argc);

static int truthy(GVal v) {
    switch (v.type) {
    case V_NUM:  return v.num != 0;
    case V_BOOL: return v.num != 0;
    case V_STR:  return v.slen > 0;
    default: return 0;
    }
}

static void out_value(catos_api_t *api, GVal v) {
    switch (v.type) {
    case V_UNDEF: out_pushs("<undef>"); break;
    case V_BOOL:  out_pushs(v.num ? "true" : "false"); break;
    case V_NUM:    out_pushn(v.num); break;
    case V_STR:   for (int i = 0; i < v.slen; i++) out_pushc(v.sp[i]); break;
    case V_FUNC:  out_pushs("<func>"); break;
    case V_ARR: {
        GArr *a = (GArr *)v.op;
        out_pushc('[');
        for (int i = 0; i < a->n; i++) { if (i) out_pushs(","); out_value(api, a->v[i]); }
        out_pushc(']');
        break;
    }
    }
}

/* resolve an lvalue node (E_ID or E_INDEX) and write a value */
static void assign_to(Node *target, GVal v) {
    if (target->op == E_ID) {
        VarE *e = scope_find(g_scope, target->sp);
        if (!e) e = scope_declare(g_globals, target->sp);
        if (!e) { rerr("out of memory"); return; }
        e->v = v;
    } else if (target->op == E_INDEX) {
        GVal arr = eval_expr(target->a);
        GVal idx = eval_expr(target->b);
        if (arr.type != V_ARR) { rerr("index on non-array"); return; }
        GArr *a = (GArr *)arr.op;
        if (idx.num < 0 || idx.num >= a->n) { rerr("array index out of range"); return; }
        a->v[idx.num] = v;
    } else {
        rerr("bad assignment target");
    }
}

static void exec_list(Node *n) {
    for (; n && !g_err && g_flow == 0; n = n->next) exec_stmt(n);
}

static void exec_stmt(Node *n) {
    if (!n || g_err || g_flow) return;
    catos_api_t *api = CE_API();
    switch (n->op) {
    case S_EMPTY: break;
    case S_BLOCK: exec_list(n->body); break;
    case S_EXPR:  eval_expr(n->a); break;
    case S_VAR:
        for (Node *d = n->a; d; d = d->next) {
            GVal v = d->a ? eval_expr(d->a) : (GVal){ V_UNDEF, 0, 0, 0, 0 };
            VarE *e = scope_declare(g_scope, d->sp);
            if (e) e->v = v;
        }
        break;
    case S_ASSIGN: {
        /* short-var := declares in current scope first */
        if (n->flag && n->a->op == E_ID) {
            VarE *e = scope_declare(g_scope, n->a->sp);
            GVal v = eval_expr(n->b);
            if (e) e->v = v;
        } else {
            GVal v = eval_expr(n->b);
            assign_to(n->a, v);
        }
        break;
    }
    case S_FUNC: {
        GFunc *f = func_new(n->sp, n->a, n->body, g_scope);
        if (!f) { rerr("out of memory"); return; }
        GVal v = { V_FUNC, 0, (const char*)n->sp, 0, f };
        VarE *e = scope_declare(g_scope, n->sp);
        if (e) e->v = v;
        break;
    }
    case S_IF:
        if (truthy(eval_expr(n->a))) exec_stmt(n->b);
        else if (n->c) exec_stmt(n->c);
        break;
    case S_FOR: {
        if (n->a) exec_stmt(n->a);
        for (;;) {
            if (g_err || g_flow) break;
            if (n->b && !truthy(eval_expr(n->b))) break;
            exec_list(n->body);
            if (g_flow == 1) { g_flow = 0; break; }
            if (g_flow == 2) { g_flow = 0; }
            if (g_flow == 3) break;
            if (n->c) exec_stmt(n->c);
        }
        break;
    }
    case S_BREAK:   g_flow = 1; break;
    case S_CONT:    g_flow = 2; break;
    case S_RETURN:  g_ret = n->a ? eval_expr(n->a) : (GVal){ V_UNDEF, 0, 0, 0, 0 }; g_flow = 3; break;
    default: rerr("bad statement"); break;
    }
    (void)api;
}

/* display length / append a value's printable form (for string concat).
 * No fixed cap: the destination is an arena buffer sized to the sum. */
static int v_disp_len(GVal v) {
    if (v.type == V_STR) return v.slen;
    if (v.type == V_NUM) {
        int n = (v.num < 0) ? 2 : 1;
        uint32_t u = v.num < 0 ? (uint32_t)(-(int64_t)v.num) : (uint32_t)v.num;
        while (u >= 10) { u /= 10; n++; }
        return n;
    }
    if (v.type == V_BOOL) return v.num ? 4 : 5;
    return 0;
}
static int v_disp(GVal v, char *o, int k) {
    int i;
    if (v.type == V_STR) { for (i = 0; i < v.slen; i++) o[k++] = v.sp[i]; return k; }
    if (v.type == V_NUM) {
        char tmp[16]; int n = 0; int neg = v.num < 0;
        uint32_t u = neg ? (uint32_t)(-(int64_t)v.num) : (uint32_t)v.num;
        if (u == 0) tmp[n++] = '0';
        while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
        if (neg) tmp[n++] = '-';
        while (n) o[k++] = tmp[--n];
        return k;
    }
    if (v.type == V_BOOL) { const char *t = v.num ? "true" : "false"; while (*t) o[k++] = *t++; return k; }
    return k;
}

static GVal eval_expr(Node *n) {
    catos_api_t *api = CE_API();
    if (!n || g_err) return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    switch (n->op) {
    case E_NUM:  return v_num(n->num);
    case E_BOOL: return v_bool(n->num);
    case E_STR:  return v_str(n->sp, n->num);
    case E_UNDEF: return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    case E_ID: {
        VarE *e = scope_find(g_scope, n->sp);
        if (!e) { rerr("undefined variable"); return (GVal){ V_UNDEF, 0, 0, 0, 0 }; }
        return e->v;
    }
    case E_INDEX: {
        GVal arr = eval_expr(n->a);
        GVal idx = eval_expr(n->b);
        if (arr.type == V_ARR) {
            GArr *a = (GArr *)arr.op;
            if (idx.num < 0 || idx.num >= a->n) { rerr("array index out of range"); return (GVal){V_UNDEF,0,0,0,0}; }
            return a->v[idx.num];
        }
        if (arr.type == V_STR) {
            if (idx.num < 0 || idx.num >= arr.slen) return v_num(0);
            return v_num((unsigned char)arr.sp[idx.num]);
        }
        rerr("index on non-indexable");
        return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    }
    case E_UN: {
        GVal x = eval_expr(n->a);
        if (n->num == '!') return v_bool(!truthy(x));
        if (n->num == OP_SUB) return v_num(-x.num);
        return x;
    }
    case E_BIN: {
        int op = n->num;
        if (op == OP_AND) { GVal l = eval_expr(n->a); if (!truthy(l)) return l; return eval_expr(n->b); }
        if (op == OP_OR)  { GVal l = eval_expr(n->a); if (truthy(l)) return l; return eval_expr(n->b); }
        GVal l = eval_expr(n->a);
        if (g_err) return (GVal){ V_UNDEF, 0, 0, 0, 0 };
        GVal r = eval_expr(n->b);
        /* string concat (either operand a string); buffer sized to the sum,
         * no fixed cap; a number/bool operand is rendered inline. */
        if (op == OP_ADD && (l.type == V_STR || r.type == V_STR)) {
            int total = v_disp_len(l) + v_disp_len(r);
            char *buf = (char *)a_alloc((uint32_t)total + 1);
            if (!buf) { rerr("out of memory"); return v_str("", 0); }
            int k = v_disp(l, buf, 0);
            k = v_disp(r, buf, k);
            buf[k] = 0;
            return v_str(buf, k);
        }
        switch (op) {
        case OP_ADD: return v_num(l.num + r.num);
        case OP_SUB: return v_num(l.num - r.num);
        case OP_MUL: return v_num(l.num * r.num);
        case OP_DIV: if (r.num == 0) { rerr("division by zero"); return v_num(0); }
                     return v_num(l.num / r.num);
        case OP_MOD: if (r.num == 0) { rerr("mod by zero"); return v_num(0); }
                     return v_num(l.num % r.num);
        case OP_EQ:
            if (l.type == V_STR && r.type == V_STR) {
                if (l.slen != r.slen) return v_bool(0);
                int eq = 1;
                for (int z = 0; z < l.slen; z++) if (l.sp[z] != r.sp[z]) eq = 0;
                return v_bool(eq);
            }
            return v_bool(l.num == r.num);
        case OP_NE:
            if (l.type == V_STR && r.type == V_STR) {
                if (l.slen != r.slen) return v_bool(1);
                int eq = 1;
                for (int z = 0; z < l.slen; z++) if (l.sp[z] != r.sp[z]) eq = 0;
                return v_bool(!eq);
            }
            return v_bool(l.num != r.num);
        case OP_LT: return v_bool(l.num < r.num);
        case OP_LE: return v_bool(l.num <= r.num);
        case OP_GT: return v_bool(l.num > r.num);
        case OP_GE: return v_bool(l.num >= r.num);
        }
        return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    }
    case E_ARRLIT: {
        GArr *a = arr_new();
        for (Node *e = n->a; e; e = e->next) {
            GVal v = eval_expr(e);
            if (a) arr_push(a, v);
        }
        GVal v = { V_ARR, 0, 0, 0, a };
        return v;
    }
    case E_CALL: {
        if (n->a->op != E_ID) { rerr("call of non-function"); return (GVal){ V_UNDEF, 0, 0, 0, 0 }; }
        VarE *e = scope_find(g_scope, n->a->sp);
        if (!e || e->v.type != V_FUNC) { rerr("call of undefined function"); return (GVal){ V_UNDEF, 0, 0, 0, 0 }; }
        GFunc *f = (GFunc *)e->v.op;
        /* collect args */
        GVal args[16]; int argc = 0;
        for (Node *a = n->b; a && argc < 16; a = a->next) args[argc++] = eval_expr(a);
        if (g_err) return (GVal){ V_UNDEF, 0, 0, 0, 0 };
        if (f->bid >= 0) return call_builtin(api, f->bid, args, argc);
        return call_user(api, f, args, argc);
    }
    default: rerr("bad expression"); return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    }
}

static GVal call_user(catos_api_t *api, GFunc *f, GVal *args, int argc) {
    Scope *saved = g_scope;
    Scope *s = scope_new(f->scope);
    if (!s) { rerr("out of memory"); return (GVal){ V_UNDEF, 0, 0, 0, 0 }; }
    int i = 0;
    for (Node *p = f->params; p && i < argc; p = p->next, i++) {
        VarE *e = scope_declare(s, p->sp);
        if (e) e->v = args[i];
    }
    g_scope = s;
    g_flow = 0;
    g_ret = (GVal){ V_UNDEF, 0, 0, 0, 0 };
    exec_list(f->body);
    GVal ret = g_ret;
    g_flow = 0;
    g_scope = saved;
    (void)api;
    return ret;
}

/* ====================================================================
 * builtins
 * ================================================================== */
static char g_pathbuf[256];
static char g_filebuf[20480];      /* source buffer (>=8KB, bss) */

static GVal call_builtin(catos_api_t *api, int bid, GVal *args, int argc) {
    switch (bid) {
    case 0: {   /* print(...) */
        out_reset();
        for (int i = 0; i < argc; i++) { if (i) out_pushc(' '); out_value(api, args[i]); }
        out_flush(api);
        return (GVal){ V_UNDEF, 0, 0, 0, 0 };
    }
    case 1: {   /* len(s | a) */
        if (argc < 1) return v_num(0);
        if (args[0].type == V_STR) return v_num(args[0].slen);
        if (args[0].type == V_ARR) return v_num(((GArr *)args[0].op)->n);
        return v_num(0);
    }
    case 2: {   /* readfile(path) -> string */
        if (argc < 1 || args[0].type != V_STR) return v_str("", 0);
        sncpy(g_pathbuf, args[0].sp, (int)sizeof(g_pathbuf) - 1);
        int got = api->file_read(g_pathbuf, g_filebuf, (int)sizeof(g_filebuf) - 1, 0);
        if (got < 0) return v_str("", 0);
        g_filebuf[got] = 0;
        return v_str(a_strn(g_filebuf, got), got);
    }
    case 3: {   /* argv(n) -> string */
        /* forwarded from ce_main: see g_argv / g_argc */
        extern char **g_os_argv; extern int g_os_argc;
        int n = argc >= 1 ? args[0].num : 0;
        if (n < 0 || n >= g_os_argc) return v_str("", 0);
        const char *a = g_os_argv[n];
        return v_str(a_str(a), slen(a));
    }
    case 4: {   /* atoi(s) -> int */
        if (argc < 1 || args[0].type != V_STR) return v_num(0);
        return v_num(api->atoi(args[0].sp));
    }
    case 5: {   /* chr(n) -> string */
        char c = (char)(args[0].num & 0xff);
        return v_str(a_strn(&c, 1), 1);
    }
    case 6: {   /* sub(s, start, n) -> string */
        if (argc < 3 || args[0].type != V_STR) return v_str("", 0);
        int st = args[1].num, ln = args[2].num;
        if (st < 0) st = 0;
        if (st + ln > args[0].slen) ln = args[0].slen - st;
        if (ln < 0) ln = 0;
        return v_str(a_strn(args[0].sp + st, ln), ln);
    }
    case 7: {   /* string(n) -> decimal string */
        char tmp[16]; out_reset(); out_pushn(args[0].num);
        sncpy(tmp, g_out, (int)sizeof(tmp) - 1);
        return v_str(a_str(tmp), slen(tmp));
    }
    case 8: {   /* append(arr, v) -> arr (grow) */
        if (argc < 2 || args[0].type != V_ARR) return args[0];
        GArr *a = (GArr *)args[0].op;
        arr_push(a, args[1]);
        return args[0];
    }
    }
    rerr("unknown builtin");
    return (GVal){ V_UNDEF, 0, 0, 0, 0 };
}

/* ====================================================================
 * built-in demo script (fib recursion + arrays + strings)
 * ================================================================== */
static const char DEMO_GO[] =
    "// CatOS go-subset demo\n"
    "func fib(n int) int {\n"
    "    if n < 2 { return n }\n"
    "    return fib(n - 1) + fib(n - 2)\n"
    "}\n"
    "func add(a int, b int) int { return a + b }\n"
    "var x = 5\n"
    "var y = x * 2 + 1\n"
    "var s = \"hello \" + \"go\"\n"
    "var a = [1, 2, 3, 4]\n"
    "var total = 0\n"
    "for i := 0; i < len(a); i = i + 1 { total = total + a[i] }\n"
    "var evens = 0\n"
    "for j := 0; j < 6; j = j + 1 {\n"
    "    if j % 2 == 0 { evens = evens + j } else { }\n"
    "}\n"
    "print(\"x=\", x, \" y=\", y, \" s=\", s)\n"
    "print(\"fib(10)=\", fib(10))\n"
    "print(\"add(3,4)=\", add(3, 4))\n"
    "print(\"total=\", total, \" evens=\", evens, \" a[2]=\", a[2])\n"
    "print(\"demo done\")\n";

/* forwarded command line (for argv(n) builtin) */
char **g_os_argv;
int g_os_argc;

/* ====================================================================
 * entry
 * ================================================================== */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    g_os_argv = argv; g_os_argc = argc;

    api->write("CatOS go subset v1.0\n");
    api->write("---------------------\n");

    /* ---- arena from kernel heap (fallback to static) ---- */
    void *heap = api->alloc(ARENA_SZ);
    g_arena = heap ? (uint8_t *)heap : g_arena_static;
    g_apos = 0; g_oom = 0; g_err = 0; g_flow = 0;

    /* ---- pick the script source ---- */
    const char *path = 0;
    if (argc >= 2 && argv[1] && argv[1][0]) path = argv[1];
    else if (api->file_exists("/BIN/DEMO.GO")) path = "/BIN/DEMO.GO";

    if (path) {
        int got = api->file_read(path, g_filebuf, (int)sizeof(g_filebuf) - 1, 0);
        if (got < 0) {
            api->write("go: cannot read script: "); api->write(path); api->write("\n");
            return 1;
        }
        g_filebuf[got] = 0;
        api->write("script: "); api->write(path);
        api->write(" ("); api->print_int(got); api->write(" bytes)\n");
        lex_run(g_filebuf, got);
    } else {
        int n = 0;
        while (DEMO_GO[n] && n < (int)sizeof(g_filebuf) - 1) { g_filebuf[n] = DEMO_GO[n]; n++; }
        g_filebuf[n] = 0;
        api->write("script: built-in demo\n");
        lex_run(g_filebuf, n);
    }

    /* ---- global scope + builtins ---- */
    g_scope = 0;
    g_globals = scope_new(0);
    if (!g_globals) { api->write("go: out of memory\n"); return 1; }
    {
        const char *names[9] = { "print", "len", "readfile", "argv",
                                 "atoi", "chr", "sub", "string", "append" };
        for (int i = 0; i < 9; i++) {
            GVal v = { V_FUNC, 0, names[i], 0, bfunc_new(names[i], i) };
            VarE *e = scope_declare(g_globals, names[i]);
            if (e) e->v = v;
        }
    }
    g_scope = g_globals;
#ifdef GO_DEBUG
    api->write("DBG globals:");
    for (int i = 0; i < g_globals->n; i++) { api->write(" ["); api->write(g_globals->v[i].name); api->write("]"); }
    api->write("\n");
#endif

    /* ---- parse ---- */
    g_ti = 0; g_perr = 0; g_errline = 0;
    Node *prog = p_stmt_list();
    if (g_oom || g_perr) {
#ifdef GO_DEBUG_TOK
        for (int z = g_ti - 8; z < g_ti + 6 && z < g_tokn; z++) {
            Tok *tz = &g_tok[z];
            api->write("["); api->print_int(z); api->write("]");
            if (tz->type == T_ID) { api->write("I:"); for (int k=0;k<tz->len;k++) api->putc(tz->sp[k]); }
            else if (tz->type == T_OP) { api->write("O:"); api->putc((char)tz->num); }
            else if (tz->type == T_NUM) { api->write("N:"); api->print_int(tz->num); }
            else if (tz->type == T_STR) { api->write("S:"); for (int k=0;k<tz->len;k++) api->putc(tz->sp[k]); }
            else api->write("E");
            api->write(" ");
        }
        api->write("\n");
#endif
        api->write("go: syntax error at line ");
        api->print_int(g_perr ? g_errline : g_line);
        api->write(" (tok#"); api->print_int(g_ti);
        api->write("/"); api->print_int(g_tokn);
        Tok *tt = cur();
        if (tt->type == T_ID) { api->write(" id="); api->write(tt->sp); }
        else if (tt->type == T_OP) { api->write(" op="); api->putc((char)tt->num); }
        else if (tt->type == T_NUM) { api->write(" num="); api->print_int(tt->num); }
        api->write(")\n");
        return 1;
    }
    if (g_ti < g_tokn - 1 || cur()->type != T_EOF) {
        api->write("go: syntax error (trailing tokens) at line ");
        api->print_int(cur()->line);
        api->write("\n");
        return 1;
    }

    /* ---- run ---- */
    exec_list(prog);
    if (g_err) {
        if (g_oom) api->write("go: out of memory\n");
        else if (g_errmsg) { api->write("go: runtime error: "); api->write(g_errmsg); api->write("\n"); }
        else api->write("go: runtime error\n");
        return 1;
    }
    api->write("go: exit ok\n");
    return 0;
}
