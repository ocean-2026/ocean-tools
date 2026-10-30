/*
 * cc.ce -- CatOS tiny C compiler (target one, task #19).
 *     run /bin/cc.ce /SRC/HELLO.C /BIN/OUT.CE
 * Compiles a C subset straight to a CatOS Executable (.ce): a flat i386
 * image with the CE01 header, loaded at 0x02000000, system-call table
 * reachable through the __ce_api slot.
 * Supported subset
 *   types   int / char / void, pointers, 1-D arrays
 *   decls   globals with constant init, locals, string literals
 *   funcs   parameters, recursion, forward calls, return
 *   stmts   blocks, if/else, while, for, break, continue, return
 *   exprs   = += -= *= /= , ?:, || && | ^ & == != < > <= >= << >>
 *           + - * / % , unary - ! ~ * & , calls, indexing
 *   api     `api->write("x");` lowers to a call through the table
 * Codegen: stack machine over EAX/ECX.  Every expression leaves its
 * value in EAX; binary operators push the left side, evaluate the right,
 * pop the left into ECX.  Calls are cdecl like the MinGW-built apps.
 * Two passes: pass 1 records function address slots (all instructions are
 * emitted in one buffer; call sites are patched at the end), pass 2 emits.
 * Build: sh extapp/build_ce.sh cc.c   -> extapp/bin/cc.ce
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* ====================================================================
 * helpers
 * ================================================================== */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void sncpy(char *d, const char *s, int n) {
    while (n-- > 0 && *s) *d++ = *s++;
    *d = 0;
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_upper(int c) { return c >= 'A' && c <= 'Z'; }

static int  g_err;
static char g_msg[64];
static void err_set(const char *m) {
    if (g_err) return;
    g_err = 1;
    sncpy(g_msg, m, (int)sizeof(g_msg) - 1);
}

/* ====================================================================
 * memory budget
 *
 * A CE program's address space puts the image and its bss inside ONE
 * 512 KiB window (USER_IMAGE_BASE .. USER_VSYSCALL), so every static byte
 * spent on buffers steals from the code budget.  The compiler's working
 * sets (source text, token stream, output image) are therefore taken
 * from the per-task heap via api->alloc(), which lives in a separate
 * 256 KiB window (USER_HEAP_BASE).  If the heap cannot serve a request we
 * fall back to the small static buffers so the compiler still runs on a
 * minimal kernel.
 * ================================================================== */
/* Heap working-set sizes.  USER_HEAP_MAX is 256 KiB total, shared
 * with anything else the compiler allocates:
 *     code 128K + source 64K + tokens 56K ~= 248K. */
#define OB_MAX (128 * 1024)
static uint8_t  g_ob_static[8 * 1024];
static uint32_t g_ob_cap = sizeof(g_ob_static);
static uint8_t *g_ob = g_ob_static;

static void *cc_alloc(catos_api_t *api, uint32_t n, void *fallback, uint32_t *cap) {
    void *p = api->alloc(n);
    if (p) { *cap = n; return p; }
    return fallback;
}

/* ---- code output buffer (heap-backed, see cc_alloc) ---- */
static uint32_t g_olen;

static uint8_t *ob_buf(void) { return g_ob; }

static void ob8(uint32_t v) { if (g_olen < g_ob_cap) g_ob[g_olen++] = (uint8_t)v; }
static void ob32(uint32_t v) { ob8(v); ob8(v >> 8); ob8(v >> 16); ob8(v >> 24); }
static uint32_t ob_at(void) { return g_olen; }
static void ob_patch(uint32_t at, uint32_t v) {
    if (at + 4 > g_olen) return;
    g_ob[at] = (uint8_t)v; g_ob[at+1] = (uint8_t)(v>>8);
    g_ob[at+2] = (uint8_t)(v>>16); g_ob[at+3] = (uint8_t)(v>>24);
}

/* ====================================================================
 * tokens
 * ================================================================== */
/* source text: heap preferred, small static fallback */
#define SRC_MAX (64 * 1024)
static char   g_src_static[8 * 1024];
static char  *g_src = g_src_static;
static uint32_t g_src_cap = sizeof(g_src_static);
static uint32_t g_slen;

enum { TK_EOF = 0, TK_ID, TK_NUM, TK_STR, TK_CHR, TK_PUNCT };

typedef struct { int kind, num, op, len, line; const char *sp; } token_t;
#define TOK_MAX (2300)        /* tokens that fit in the 56 KiB slot */
static token_t  g_tok_static[512];
static token_t *g_tok = g_tok_static;
static int      g_tok_cap = 512;
static int g_ntok, g_ti;

static const char *const PUNCTS[] = {
    "<<=", ">>=", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", "<<", ">>",
    "(", ")", "{", "}", "[", "]", ";", ",", ".", "?", ":", "=",
    "+", "-", "*", "/", "%", "<", ">", "!", "~", "&", "|", "^",
    0
};
/* token op == index into PUNCTS; stable names for code */
enum {
    OP_SHLASG = 0, OP_SHRASG, OP_EQ, OP_NE, OP_LE, OP_GE, OP_ANDAND, OP_OROR,
    OP_INC, OP_DEC, OP_PLUSASG, OP_MINUSASG, OP_MULASG, OP_DIVASG, OP_MODASG,
    OP_ANDASG, OP_ORASG, OP_XORASG, OP_ARROW, OP_SHL, OP_SHR,
    OP_LP, OP_RP, OP_LB, OP_RB, OP_LSB, OP_RSB, OP_SEMI, OP_COMMA, OP_DOT,
    OP_QMARK, OP_COLON, OP_ASG, OP_PLUS, OP_MINUS, OP_STAR, OP_SLASH,
    OP_PERCENT, OP_LT, OP_GT, OP_NOT, OP_TILDE, OP_AMP, OP_PIPE, OP_CARET
};

static int tok_op(void) { return g_tok[g_ti].op; }
static int at_op(int op) { return g_tok[g_ti].kind == TK_PUNCT && g_tok[g_ti].op == op; }
static int accept_op(int op) { if (at_op(op)) { g_ti++; return 1; } return 0; }
static token_t *cur(void) { return &g_tok[g_ti]; }
static token_t *peek(int n) { int i = g_ti + n; if (i > g_ntok) i = g_ntok; return &g_tok[i]; }
static int at_kw(const char *kw) {
    token_t *t = cur();
    int n = slen(kw);
    if (t->kind != TK_ID || t->len != n) return 0;
    for (int i = 0; i < n; i++) if (t->sp[i] != kw[i]) return 0;
    return 1;
}
static int accept_kw(const char *kw) { if (at_kw(kw)) { g_ti++; return 1; } return 0; }

static int tokenize(void) {
    uint32_t i = 0;
    int line = 1;
    g_ntok = 0;
    while (i < g_slen) {
        int c = (unsigned char)g_src[i];
        if (c == '\n') { line++; i++; continue; }
        if (is_space(c)) { i++; continue; }
        if (c == '/' && i + 1 < g_slen && g_src[i+1] == '/') {
            while (i < g_slen && g_src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < g_slen && g_src[i+1] == '*') {
            i += 2;
            while (i + 1 < g_slen && !(g_src[i] == '*' && g_src[i+1] == '/')) {
                if (g_src[i] == '\n') line++;
                i++;
            }
            i += 2;
            continue;
        }
        if (g_ntok >= g_tok_cap - 1) { err_set("source too complex"); return 0; }
        token_t *t = &g_tok[g_ntok];
        t->line = line; t->len = 0; t->num = 0; t->op = -1;
        if (is_alpha(c)) {
            uint32_t s = i;
            while (i < g_slen && is_alnum((unsigned char)g_src[i])) i++;
            t->kind = TK_ID; t->sp = &g_src[s]; t->len = (int)(i - s);
            g_ntok++; continue;
        }
        if (is_digit(c)) {
            int v = 0;
            if (c == '0' && i + 1 < g_slen && (g_src[i+1] == 'x' || g_src[i+1] == 'X')) {
                i += 2;
                for (;;) {
                    int d = (unsigned char)g_src[i], nv = -1;
                    if (i >= g_slen) break;
                    if (d >= '0' && d <= '9') nv = d - '0';
                    else if (d >= 'a' && d <= 'f') nv = d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') nv = d - 'A' + 10;
                    if (nv < 0) break;
                    v = v * 16 + nv; i++;
                }
            } else {
                while (i < g_slen && is_digit((unsigned char)g_src[i]))
                    v = v * 10 + (g_src[i++] - '0');
            }
            t->kind = TK_NUM; t->num = v; g_ntok++; continue;
        }
        if (c == '\'') {
            int v = 0;
            i++;
            if (i < g_slen && g_src[i] == '\\') {
                i++;
                int e = (unsigned char)g_src[i++];
                switch (e) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0; break;
                case '\\': v = '\\'; break;
                case '\'': v = '\''; break;
                default: v = e; break;
                }
            } else if (i < g_slen) v = (unsigned char)g_src[i++];
            while (i < g_slen && g_src[i] != '\'') i++;
            if (i < g_slen) i++;
            t->kind = TK_CHR; t->num = v; g_ntok++; continue;
        }
        if (c == '"') {
            uint32_t s = ++i;
            while (i < g_slen && g_src[i] != '"') {
                if (g_src[i] == '\\' && i + 1 < g_slen) i++;
                i++;
            }
            t->kind = TK_STR; t->sp = &g_src[s]; t->len = (int)(i - s);
            if (i < g_slen) i++;
            g_ntok++; continue;
        }
        {
            int best = -1, bn = 0;
            for (int n = 3; n >= 1; n--) {
                if (i + (uint32_t)n > g_slen) continue;
                for (int k = 0; PUNCTS[k]; k++) {
                    int L = slen(PUNCTS[k]);
                    if (L != n) continue;
                    int m = 0;
                    while (m < n && PUNCTS[k][m] == g_src[i + m]) m++;
                    if (m == n) { best = k; bn = n; break; }
                }
                if (best >= 0) break;
            }
            if (best < 0) { err_set("bad character"); return 0; }
            t->kind = TK_PUNCT; t->op = best; t->len = bn;
            i += bn; g_ntok++; continue;
        }
    }
    g_tok[g_ntok].kind = TK_EOF;
    g_tok[g_ntok].op = -1;
    g_tok[g_ntok].num = 0;
    g_tok[g_ntok].len = 0;
    g_tok[g_ntok].sp = &g_src[g_slen];
    g_tok[g_ntok].line = line;
    return 1;
}

/* ====================================================================
 * symbols
 * ================================================================== */
#define SYM_MAX 96
#define LOC_MAX 32

enum { SY_FUNC = 1, SY_GVAR, SY_LVAR };

typedef struct {
    char name[28];
    int  kind, off, sz, is_ptr, is_arr, arr_sz;
    int  addr, nparams;
} sym_t;

static sym_t g_sym[SYM_MAX];
static int   g_nsym;
static sym_t *g_loc[LOC_MAX];
static int    g_nloc;
static int    g_frame;

/* data section (strings + globals), copied in after code */
static uint8_t  g_data[8 * 1024];
static uint32_t g_dlen;

/* relocations in the code: 32-bit immediates holding a data offset */
#define REL_MAX 128
static int g_rel_at[REL_MAX];
static int g_rel_off[REL_MAX];
static int g_nrel;

/* call sites: absolute address of the callee resolved at the end */
#define CALLMAX 128
static int    g_call_at[CALLMAX];
static int    g_call_sym[CALLMAX];
static int    g_ncall;

/* api slot loads to patch: mov eax,[imm32] with the api offset */
#define APIREF_MAX 64
static int g_apiref[APIREF_MAX];
static int g_napiref;

static int name_eqn(const char *a, const char *b, int nb) {
    int k = 0;
    while (k < nb && a[k] && a[k] == b[k]) k++;
    return k == nb && a[k] == 0;
}
static sym_t *gfind(const char *n, int len) {
    for (int i = 0; i < g_nsym; i++)
        if (name_eqn(g_sym[i].name, n, len)) return &g_sym[i];
    return 0;
}
static sym_t *lfind(const char *n, int len) {
    for (int i = g_nloc - 1; i >= 0; i--)
        if (name_eqn(g_loc[i]->name, n, len)) return g_loc[i];
    return 0;
}
static sym_t *sym_new(const char *n, int len, int kind) {
    if (g_nsym >= SYM_MAX) { err_set("too many symbols"); return 0; }
    sym_t *s = &g_sym[g_nsym++];
    unsigned i;
    for (i = 0; i < sizeof(*s); i++) ((unsigned char *)s)[i] = 0;
    if (len > 27) len = 27;
    sncpy(s->name, n, len);
    s->kind = kind;
    s->sz = 4;
    return s;
}

static const char *const API_NAMES[] = {
    "write", "putc", "cls", "readline", "time_str", "fs_ready", "ls",
    "touch", "mkdir", "cat", "save", "alloc", "free", "exit",
    "print_int", "print_hex", "set_fg", "delay", "atoi", "strlen",
    "strcmp", "strcpy", "getch", "file_exists", "fsize", "net_init",
    "net_up", "http_get", "net_dns", "net_udp_send", "net_udp_recv",
    "net_ping", "net_ip", "getuid", "gettid", "kill", "yield",
    "hw_num", "hw_str", "file_read", "file_write", "file_delete",
    "pipe_create", "pipe_write", "pipe_read", "pipe_close", "spawn", "wait",
    0
};
static int api_slot(token_t *t) {
    int i;
    for (i = 0; API_NAMES[i]; i++)
        if (name_eqn(API_NAMES[i], t->sp, t->len)) return i;
    return -1;
}

/* ====================================================================
 * x86 emitter
 * ================================================================== */
static void e_mov_eax(int v) { ob8(0xB8); ob32((uint32_t)v); }
static void e_lea_eax_ebp(int off) { ob8(0x8D); ob8(0x85); ob32((uint32_t)off); }
static void e_mov_eax_ebp(int off) { ob8(0x8B); ob8(0x85); ob32((uint32_t)off); }
static void e_movsx_eax_ebp(int off) { ob8(0x0F); ob8(0xBE); ob8(0x85); ob32((uint32_t)off); }
static void e_mov_ebp_eax(int off) { ob8(0x89); ob8(0x85); ob32((uint32_t)off); }
static void e_mov8_ebp_eax(int off) { ob8(0x88); ob8(0x85); ob32((uint32_t)off); }
static void e_ld_eax_ptr(void) { ob8(0x8B); ob8(0x00); }
static void e_ldsx_eax_ptr(void) { ob8(0x0F); ob8(0xBE); ob8(0x00); }
static void e_st_eax_ptr(void) { ob8(0x89); ob8(0x08); }     /* mov [eax],ecx */
static void e_st8_eax_ptr(void) { ob8(0x88); ob8(0x08); }    /* mov [eax],cl  */
static void e_push_eax(void) { ob8(0x50); }
static void e_push_ecx(void) { ob8(0x51); }
static void e_pop_eax(void) { ob8(0x58); }
static void e_pop_ecx(void) { ob8(0x59); }
static void e_pop_edx(void) { ob8(0x5A); }
static void e_mov_eax_ecx(void) { ob8(0x89); ob8(0xC8); }
static void e_mov_ecx_eax(void) { ob8(0x89); ob8(0xC1); }
static void e_test_eax(void) { ob8(0x85); ob8(0xC0); }
static void e_neg_eax(void) { ob8(0xF7); ob8(0xD8); }
static void e_not_eax(void) { ob8(0xF7); ob8(0xD0); }
static void e_setcc(int cc) {
    ob8(0x0F); ob8(0x90 | cc); ob8(0xC0);
    ob8(0x0F); ob8(0xB6); ob8(0xC0);
}
static void e_test_setcc(int cc) { e_test_eax(); e_setcc(cc); }
static void e_add_esp(int n) { if (n) { ob8(0x81); ob8(0xC4); ob32((uint32_t)n); } }
static void e_sub_esp(int n) { if (n) { ob8(0x81); ob8(0xEC); ob32((uint32_t)n); } }
static void e_bin_eax_ecx(int op) { ob8(op); ob8(0xC8); }   /* op eax, ecx */
static void e_cmp_eax_ecx(void) { ob8(0x39); ob8(0xC1); }   /* cmp ecx,eax */

/* forward jumps */
static int e_jmp_fwd(void)  { ob8(0xE9); int a = (int)ob_at(); ob32(0); return a; }
static int e_jz_fwd(void)   { ob8(0x0F); ob8(0x84); int a = (int)ob_at(); ob32(0); return a; }
static int e_jnz_fwd(void)  { ob8(0x0F); ob8(0x85); int a = (int)ob_at(); ob32(0); return a; }
static void e_patch_here(int at) { ob_patch((uint32_t)at, ob_at() - (uint32_t)at - 4); }
static void e_jmp_back(int target) { ob8(0xE9); ob32((uint32_t)(target - (int)ob_at() - 4)); }
static void e_jcc_back(int cc, int target) {
    ob8(0x0F); ob8(0x80 | cc); ob32((uint32_t)(target - (int)ob_at() - 4));
}

/* ====================================================================
 * expression codegen
 * ================================================================== */
static void gen_expr(void);
static void gen_assign_expr(void);

static sym_t *local_new(const char *n, int len, int sz, int is_ptr, int is_arr, int arr_sz) {
    if (g_nloc >= LOC_MAX) { err_set("too many locals"); return 0; }
    sym_t *s = sym_new(n, len, SY_LVAR);
    if (!s) return 0;
    int unit = is_ptr ? 4 : sz;
    int bytes = is_arr ? arr_sz * unit : 4;
    if (bytes < 4) bytes = 4;
    g_frame += bytes;
    s->off = -g_frame;
    s->sz = sz;
    s->is_ptr = is_ptr;
    s->is_arr = is_arr;
    s->arr_sz = arr_sz;
    g_loc[g_nloc++] = s;
    return s;
}

/* helper: emit `mov eax, imm32` with a relocation to a data offset */
static void e_data_addr(int data_off) {
    ob8(0xB8); ob32(0);
    if (g_nrel < REL_MAX) { g_rel_at[g_nrel] = (int)ob_at() - 4; g_rel_off[g_nrel] = data_off; g_nrel++; }
}

/* load variable value / address */
static void gen_load(sym_t *s, int want_addr) {
    if (s->kind == SY_LVAR) {
        if (want_addr) { e_lea_eax_ebp(s->off); return; }
        if (s->is_arr) { e_lea_eax_ebp(s->off); return; }
        if (s->is_ptr) { e_mov_eax_ebp(s->off); return; }
        if (s->sz == 1) e_movsx_eax_ebp(s->off);
        else e_mov_eax_ebp(s->off);
        return;
    }
    if (want_addr || s->is_arr) { e_data_addr(s->addr); return; }
    if (s->is_ptr) { e_data_addr(s->addr); e_ld_eax_ptr(); return; }  /* pointer value */
    e_data_addr(s->addr);
    if (s->sz == 1) e_ldsx_eax_ptr();
    else e_ld_eax_ptr();
}

/* push args right-to-left: we evaluate left-to-right and reverse */
static int push_args(void) {
    int n = 0;
    if (!at_op(OP_RP)) {
        for (;;) {
            gen_assign_expr();
            e_push_eax();
            n++;
            if (!accept_op(OP_COMMA)) break;
        }
    }
    if (!accept_op(OP_RP)) err_set("expected )");
    for (int i = 0; i < n / 2; i++) {
        int o1 = i * 4, o2 = (n - 1 - i) * 4;
        ob8(0x8B); ob8(0x44); ob8(0x24); ob8((uint32_t)o1);
        ob8(0x8B); ob8(0x4C); ob8(0x24); ob8((uint32_t)o2);
        ob8(0x89); ob8(0x4C); ob8(0x24); ob8((uint32_t)o1);
        ob8(0x89); ob8(0x44); ob8(0x24); ob8((uint32_t)o2);
    }
    return n;
}

/* the size of the element an [index] expression yields */
static int elem_sz(sym_t *s) {
    /* a pointer occupies 4 bytes; an array element occupies sizeof(*p) */
    if (s->is_ptr) return 4;
    if (s->is_arr) return s->sz;
    return s->sz;
}

/* emit `base + index*esize` into EAX */
static void gen_addr_indexed(sym_t *s) {
    gen_load(s, 1);                    /* base address */
    e_push_eax();
    gen_expr();
    int es = elem_sz(s);
    if (es != 1) { ob8(0x69); ob8(0xC0); ob32((uint32_t)es); }  /* imul eax,eax,es */
    e_pop_ecx();
    ob8(0x01); ob8(0xC1);              /* add ecx,eax */
    e_mov_eax_ecx();
}

/* postfix expression: literal / name / call / index */
static void gen_postfix(void) {
    token_t *t = cur();
    if (t->kind == TK_NUM || t->kind == TK_CHR) {
        e_mov_eax(t->num);
        g_ti++;
        return;
    }
    if (t->kind == TK_STR) {
        int o = (int)g_dlen;
        for (int i = 0; i < t->len && g_dlen < sizeof(g_data) - 1; i++) {
            char c = t->sp[i];
            if (c == '\\' && i + 1 < t->len) {
                char e = t->sp[++i];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '0': c = 0; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                default: c = e; break;
                }
            }
            g_data[g_dlen++] = (uint8_t)c;
        }
        if (g_dlen < sizeof(g_data)) g_data[g_dlen++] = 0;
        e_data_addr(o);
        g_ti++;
        return;
    }
    if (accept_op(OP_LP)) {
        gen_expr();
        if (!accept_op(OP_RP)) err_set("expected )");
        return;
    }
    if (t->kind != TK_ID) { err_set("expression expected"); return; }

    /* api -> name ( ... ) */
    if (peek(0)->kind == TK_ID && name_eqn("api", t->sp, t->len) &&
        peek(1)->kind == TK_PUNCT && peek(1)->op == OP_ARROW) {
        int slot = api_slot(peek(2));
        if (slot < 0) { err_set("unknown api function"); return; }
        g_ti += 3;
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int n = push_args();
        ob8(0xA1); ob32(0);                    /* mov eax,[api_slot] */
        if (g_napiref < APIREF_MAX) g_apiref[g_napiref++] = (int)ob_at() - 4;
        ob8(0xFF); ob8(0x50); ob8((uint32_t)(slot * 4));
        e_add_esp(n * 4);
        return;
    }

    sym_t *s = lfind(t->sp, t->len);
    if (!s) s = gfind(t->sp, t->len);
    if (!s) { err_set("undeclared name"); return; }
    g_ti++;

    if (s->kind == SY_FUNC) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int n = push_args();
        ob8(0xE8); ob32(0);
        if (g_ncall < CALLMAX) {
            g_call_at[g_ncall] = (int)ob_at() - 4;
            g_call_sym[g_ncall] = (int)(s - g_sym);
            g_ncall++;
        }
        e_add_esp(n * 4);
        return;
    }
    if (at_op(OP_LSB)) {
        g_ti++;
        gen_addr_indexed(s);
        if (elem_sz(s) == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();
        if (!accept_op(OP_RSB)) err_set("expected ]");
        return;
    }
    gen_load(s, 0);
}

static void gen_unary(void) {
    int o = tok_op();
    if (g_tok[g_ti].kind == TK_PUNCT) {
        if (o == OP_NOT)   { g_ti++; gen_unary(); e_test_setcc(0x4); return; }
        if (o == OP_MINUS) { g_ti++; gen_unary(); e_neg_eax(); return; }
        if (o == OP_TILDE) { g_ti++; gen_unary(); e_not_eax(); return; }
        if (o == OP_AMP)   { g_ti++; gen_unary(); return; }   /* address */
        if (o == OP_STAR)  { g_ti++; gen_unary(); e_ld_eax_ptr(); return; }
        if (o == OP_PLUS)  { g_ti++; gen_unary(); return; }
    }
    gen_postfix();
}

static int binop_prec(void) {
    if (g_tok[g_ti].kind != TK_PUNCT) return -1;
    switch (g_tok[g_ti].op) {
    case OP_OROR:    return 2;
    case OP_ANDAND:  return 3;
    case OP_PIPE:    return 4;
    case OP_CARET:   return 5;
    case OP_AMP:     return 6;
    case OP_EQ: case OP_NE: return 7;
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 8;
    case OP_SHL: case OP_SHR: return 9;
    case OP_PLUS: case OP_MINUS: return 10;
    case OP_STAR: case OP_SLASH: case OP_PERCENT: return 11;
    default: return -1;
    }
}

static void gen_binary(int minprec) {
    gen_unary();
    for (;;) {
        int prec = binop_prec();
        if (prec < minprec) break;
        int op = g_tok[g_ti].op;
        g_ti++;
        if (op == OP_ANDAND) {
            e_test_eax();
            int j = e_jz_fwd();
            gen_binary(prec + 1);
            e_test_setcc(0x5);             /* setne */
            int e = e_jmp_fwd();
            e_patch_here(j);
            e_mov_eax(0);
            e_patch_here(e);
            continue;
        }
        if (op == OP_OROR) {
            e_test_eax();
            int j = e_jnz_fwd();
            gen_binary(prec + 1);
            e_test_setcc(0x5);
            int e = e_jmp_fwd();
            e_patch_here(j);
            e_mov_eax(1);
            e_patch_here(e);
            continue;
        }
        e_push_eax();
        gen_binary(prec + 1);
        e_pop_ecx();                        /* ecx = left, eax = right */
        switch (op) {
        case OP_PLUS:    e_bin_eax_ecx(0x01); break;              /* add eax,ecx */
        case OP_MINUS:   ob8(0x29); ob8(0xC1); e_mov_eax_ecx(); break; /* sub ecx,eax */
        case OP_STAR:    ob8(0x0F); ob8(0xAF); ob8(0xC1); break;  /* imul eax,ecx */
        /* left is in ecx, right in eax: xchg puts the right side in ecx */
        case OP_SLASH:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9); break;
        case OP_PERCENT: ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9);
                         ob8(0x89); ob8(0xD0); break;             /* mov eax,edx */
        case OP_AMP:     e_bin_eax_ecx(0x21); break;
        case OP_PIPE:    e_bin_eax_ecx(0x09); break;
        case OP_CARET:   e_bin_eax_ecx(0x31); break;
        case OP_SHL:     e_mov_eax_ecx(); ob8(0xD3); ob8(0xE0); break;
        case OP_SHR:     e_mov_eax_ecx(); ob8(0xD3); ob8(0xF8); break;
        case OP_EQ: e_cmp_eax_ecx(); e_setcc(0x4); break;
        case OP_NE: e_cmp_eax_ecx(); e_setcc(0x5); break;
        case OP_LT: e_cmp_eax_ecx(); e_setcc(0xC); break;
        case OP_GT: e_cmp_eax_ecx(); e_setcc(0xF); break;
        case OP_LE: e_cmp_eax_ecx(); e_setcc(0xE); break;
        case OP_GE: e_cmp_eax_ecx(); e_setcc(0xD); break;
        default: err_set("bad operator"); return;
        }
    }
}

/* assignment: `lvalue OP= rhs` */
static void gen_assign_expr(void) {
    token_t *t = cur();
    if (t->kind == TK_ID) {
        /* find the operator after [optional index] */
        int la = 1;
        if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_LSB) {
            int depth = 1;
            la++;
            while (peek(la)->kind != TK_EOF && depth) {
                if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_LSB) depth++;
                else if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_RSB) depth--;
                la++;
            }
        }
        token_t *nx = peek(la);
        if (nx->kind == TK_PUNCT) {
            int o = nx->op;
            if (o == OP_ASG || o == OP_PLUSASG || o == OP_MINUSASG ||
                o == OP_MULASG || o == OP_DIVASG || o == OP_MODASG) {
                sym_t *s = lfind(t->sp, t->len);
                if (!s) s = gfind(t->sp, t->len);
                if (s && s->kind != SY_FUNC) {
                    /* consume the NAME (the operator is consumed below) */
                    g_ti++;
                    int es = elem_sz(s);
                    int indexed = 0;
                    if (at_op(OP_LSB)) {
                        indexed = 1;
                        g_ti++;
                        gen_addr_indexed(s);
                        if (!accept_op(OP_RSB)) err_set("expected ]");
                    } else {
                        gen_load(s, 1);
                    }
                    e_push_eax();                    /* keep lvalue address */
                    if (o == OP_ASG) {
                        g_ti++;
                        gen_assign_expr();           /* rhs -> eax */
                        e_mov_ecx_eax();             /* ecx = value */
                        e_pop_eax();                 /* eax = address */
                        if (es == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                        e_mov_eax_ecx();             /* result = value */
                        return;
                    }
                    /* compound: old = [addr]; compute; store */
                    ob8(0x8B); ob8(0x04); ob8(0x24); /* mov eax,[esp] : address */
                    if (es == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();
                    e_mov_ecx_eax();                 /* ecx = old */
                    e_push_ecx();
                    g_ti++;
                    gen_assign_expr();               /* eax = rhs */
                    e_pop_ecx();                     /* ecx = old */
                    switch (o) {
                    case OP_PLUSASG:  e_bin_eax_ecx(0x01); break;
                    case OP_MINUSASG: ob8(0x29); ob8(0xC1); e_mov_eax_ecx(); break;
                    case OP_MULASG:   ob8(0x0F); ob8(0xAF); ob8(0xC1); break;
                    /* eax = rhs, ecx = old value: xchg makes ecx the divisor */
                    case OP_DIVASG:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9); break;
                    case OP_MODASG:   ob8(0x87); ob8(0xC8); ob8(0x99); ob8(0xF7); ob8(0xF9);
                                      ob8(0x89); ob8(0xD0); break;
                    }
                    e_pop_ecx();                     /* ecx = address */
                    e_push_eax();                    /* result */
                    e_mov_eax_ecx();                 /* eax = address */
                    ob8(0x8B); ob8(0x0C); ob8(0x24); /* mov ecx,[esp] : result */
                    if (es == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                    e_pop_eax();                     /* result */
                    return;
                }
            }
        }
    }
    gen_binary(1);
    if (at_op(OP_QMARK)) {                 /* ternary: a ? b : c */
        g_ti++;
        e_test_eax();
        int j = e_jz_fwd();
        gen_assign_expr();
        int e = e_jmp_fwd();
        e_patch_here(j);
        if (!accept_op(OP_COLON)) err_set("expected :");
        gen_assign_expr();
        e_patch_here(e);
    }
}

static void gen_expr(void) {
    gen_assign_expr();
    while (at_op(OP_COMMA)) { g_ti++; gen_assign_expr(); }
}

/* ====================================================================
 * statements
 * ================================================================== */
#define BRK_MAX 64
static int g_brk[BRK_MAX];   static int g_nbrk;
static int g_cont[BRK_MAX];  static int g_ncont;
static int g_brk_base, g_cont_base;

static int is_type_kw(void) {
    return at_kw("int") || at_kw("char") || at_kw("void") || at_kw("unsigned");
}

static int parse_type(int *sz, int *is_ptr) {
    *sz = 4; *is_ptr = 0;
    if (accept_kw("char")) *sz = 1;
    else if (accept_kw("int")) *sz = 4;
    else if (accept_kw("unsigned")) { *sz = 4; accept_kw("int"); }
    else if (accept_kw("void")) *sz = 4;
    else return 0;
    while (accept_op(OP_STAR)) *is_ptr = 1;
    return 1;
}

static int const_size(void) {
    int neg = 0;
    if (accept_op(OP_MINUS)) neg = 1;
    if (cur()->kind != TK_NUM) { err_set("array size must be a number"); return 0; }
    int v = cur()->num;
    g_ti++;
    return neg ? -v : v;
}

static void gen_local_decl(void) {
    int sz, ptr, is_arr = 0, arr_sz = 0;
    if (!parse_type(&sz, &ptr)) { err_set("type expected"); return; }
    for (;;) {
        token_t *nm = cur();
        if (nm->kind != TK_ID) { err_set("name expected"); return; }
        g_ti++;
        is_arr = 0; arr_sz = 0;
        if (accept_op(OP_LSB)) {
            arr_sz = const_size();
            if (!arr_sz) arr_sz = 16;
            if (!accept_op(OP_RSB)) err_set("expected ]");
            is_arr = 1;
        }
        sym_t *s = local_new(nm->sp, nm->len, sz, ptr, is_arr, arr_sz);
        if (!s) return;
        if (accept_op(OP_ASG)) {
            /* zero the array, or store the scalar */
            gen_assign_expr();
            e_push_eax();
            e_lea_eax_ebp(s->off);
            e_pop_ecx();
            if (!is_arr && sz == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
        }
        if (!accept_op(OP_COMMA)) break;
    }
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
}

static void gen_stmt(void) {
    if (g_err) return;
    if (at_op(OP_LB)) { g_ti++; while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) { if (accept_op(OP_SEMI)) continue; gen_stmt(); } g_ti++; return; }
    if (accept_op(OP_SEMI)) return;

    if (accept_kw("return")) {
        if (!at_op(OP_SEMI)) gen_expr(); else e_mov_eax(0);
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        ob8(0x89); ob8(0xEC); ob8(0x5D); ob8(0xC3);      /* mov esp,ebp; pop ebp; ret */
        return;
    }
    if (accept_kw("if")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_test_eax();
        int j = e_jz_fwd();
        gen_stmt();
        if (accept_kw("else")) {
            int e = e_jmp_fwd();
            e_patch_here(j);
            gen_stmt();
            e_patch_here(e);
        } else e_patch_here(j);
        return;
    }
    if (accept_kw("while")) {
        int top = (int)ob_at();
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_test_eax();
        int j = e_jz_fwd();
        int bb = g_nbrk, cb = g_ncont;
        gen_stmt();
        e_jmp_back(top);
        e_patch_here(j);
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        for (int i = cb; i < g_ncont; i++) { ob_patch((uint32_t)g_cont[i], (uint32_t)(top - g_cont[i] - 4)); }
        g_ncont = cb;
        return;
    }
    if (accept_kw("for")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        if (!at_op(OP_SEMI)) gen_expr();
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        int top = (int)ob_at();
        int jz = -1;
        if (!at_op(OP_SEMI)) { gen_expr(); e_test_eax(); jz = e_jz_fwd(); }
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        /* The step clause is written before the body but must execute
         * after it.  Emit it first and jump over it from `top`; the body
         * then falls through to the step; the step jumps back to `top`:
         *   top: cond / jz end / jmp body
         *   body: <body> / jmp step
         *   step: <step code> / jmp top
         *   end: */
        int has_step = !at_op(OP_RP);
        int jbody = -1;
        if (has_step) jbody = e_jmp_fwd();      /* top -> body */
        int step_pos = (int)ob_at();
        int bb = g_nbrk, cb = g_ncont;
        if (has_step) gen_expr();               /* step code (cursor ends at ')') */
        if (has_step) e_jmp_back(top);          /* step -> cond */
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        if (jbody >= 0) e_patch_here(jbody);    /* body starts here */
        gen_stmt();                             /* body */
        e_jmp_back(step_pos);                   /* body -> step */
        if (jz >= 0) e_patch_here(jz);
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        g_ncont = cb;
        return;
    }
    if (accept_kw("break")) {
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
        return;
    }
    if (accept_kw("continue")) {
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        if (g_ncont < BRK_MAX) g_cont[g_ncont++] = e_jmp_fwd();
        return;
    }
    if (is_type_kw()) { gen_local_decl(); return; }

    gen_expr();
    if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
}

/* ====================================================================
 * top level
 * ================================================================== */
/* address of the __ce_api slot inside the image (fixed after codegen) */
static int g_api_slot = -1;
static uint32_t g_entry_off;          /* image offset of ce_main (header entry) */
static int g_entry_jmp = -1;          /* stub jmp at code offset 0 -> ce_main */

static void parse_func(void) {
    /* caller consumed the return type and name, cursor is at '(' */
    g_ti++;                                   /* ( */
    g_nloc = 0;
    g_frame = 0;
    int np = 0;
    if (!at_op(OP_RP)) {
        for (;;) {
            int psz = 4, pp = 0;
            if (!parse_type(&psz, &pp)) { accept_kw("int"); }
            token_t *pn = cur();
            if (pn->kind != TK_ID) { err_set("parameter expected"); return; }
            g_ti++;
            sym_t *ps = local_new(pn->sp, pn->len, psz, pp, 0, 0);
            if (ps) ps->off = 8 + np * 4;
            np++;
            if (accept_op(OP_COMMA)) continue;
            break;
        }
    }
    if (!accept_op(OP_RP)) err_set("expected )");
    if (accept_kw(";")) return;               /* prototype */

    /* prologue: push ebp / mov ebp,esp / sub esp,frame */
    ob8(0x55); ob8(0x89); ob8(0xE5);
    uint32_t sub_at = ob_at();
    ob8(0x81); ob8(0xEC); ob32(0);
    if (!at_op(OP_LB)) { err_set("expected {""); return; }
    gen_stmt();
    e_mov_eax(0);
    ob8(0x89); ob8(0xEC); ob8(0x5D); ob8(0xC3);
    ob_patch(sub_at + 2, (uint32_t)((g_frame + 15) & ~15u));
}

static void parse_program(void) {
    /* The kernel's CE loader demands entry == 0 and api_off + 4 <= image_size.
     * ce_main may be defined anywhere in the source, so we plant a 5-byte
     * `jmp ce_main` stub at code offset 0 (= image offset 0 after the 20-byte
     * header); the header entry stays 0 and the stub is patched in fixup_code. */
    g_entry_jmp = e_jmp_fwd();
    while (cur()->kind != TK_EOF && !g_err) {
        /* skip preprocessor lines already removed; skip `typedef`/`#` */
        if (accept_op(OP_SEMI)) continue;
        int sz, ptr;
        int is_func = 0;
        if (!parse_type(&sz, &ptr)) {
            token_t *t = cur();
            if (t->kind == TK_EOF) break;
            if (t->kind == TK_ID) { g_ti++; continue; }  /* unknown type: skip */
            err_set("type expected");
            return;
        }
        for (;;) {
            token_t *nm = cur();
            if (nm->kind != TK_ID) { err_set("name expected"); return; }
            g_ti++;
            int is_arr = 0, arr_sz = 0;
            if (accept_op(OP_LSB)) {
                arr_sz = const_size();
                if (!arr_sz) arr_sz = 16;
                if (!accept_op(OP_RSB)) err_set("expected ]");
                is_arr = 1;
            }
            if (at_op(OP_LP) && !is_arr) {
                /* remember ce_main: the kernel jumps to the header entry */
                if (name_eqn("ce_main", nm->sp, nm->len)) g_entry_off = (uint32_t)ob_at();
                sym_t *f = sym_new(nm->sp, nm->len, SY_FUNC);
                if (!f) return;
                f->addr = (int)ob_at();
                is_func = 1;
                parse_func();
                break;                        /* function: no comma list */
            }            sym_t *g = sym_new(nm->sp, nm->len, SY_GVAR);
            if (!g) return;
            g->sz = sz; g->is_ptr = ptr; g->is_arr = is_arr; g->arr_sz = arr_sz;
            g->addr = (int)g_dlen;
            {
                int unit = ptr ? 4 : sz;
                int bytes = is_arr ? arr_sz * unit : 4;
                if (bytes < 4) bytes = 4;
                for (int i = 0; i < bytes && g_dlen < sizeof(g_data); i++) g_data[g_dlen++] = 0;
            }
            if (accept_op(OP_ASG)) {
                if (cur()->kind == TK_STR) {
                    int o = (int)g_dlen;
                    token_t *st = cur();
                    for (int i = 0; i < st->len && g_dlen < sizeof(g_data) - 1; i++) {
                        char c = st->sp[i];
                        if (c == '\\' && i + 1 < st->len) {
                            char e = st->sp[++i];
                            switch (e) {
                            case 'n': c = '\n'; break;
                            case 't': c = '\t'; break;
                            default: c = e; break;
                            }
                        }
                        g_data[g_dlen++] = (uint8_t)c;
                    }
                    if (g_dlen < sizeof(g_data)) g_data[g_dlen++] = 0;
                    g_ti++;
                    uint32_t base = (uint32_t)g->addr;
                    g_data[base] = (uint8_t)o; g_data[base+1] = (uint8_t)(o>>8);
                    g_data[base+2] = (uint8_t)(o>>16); g_data[base+3] = (uint8_t)(o>>24);
                } else {
                    int neg = 0;
                    if (accept_op(OP_MINUS)) neg = 1;
                    int v = 0;
                    if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) { v = cur()->num; g_ti++; }
                    else { err_set("constant initialiser expected"); return; }
                    if (neg) v = -v;
                    uint32_t base = (uint32_t)g->addr;
                    g_data[base] = (uint8_t)v;
                    if (!is_arr) {
                        g_data[base+1] = (uint8_t)(v>>8);
                        g_data[base+2] = (uint8_t)(v>>16);
                        g_data[base+3] = (uint8_t)(v>>24);
                    }
                }
            }
            if (accept_op(OP_COMMA)) continue;
            break;
        }
        if (is_func) continue;                     /* a function body has no trailing ; */
        if (!accept_op(OP_SEMI)) {
            if (!g_err) err_set("expected ; after declaration");
            return;
        }
    }
}

/* ====================================================================
 * final image assembly
 * Layout inside the CE image (loaded at CE_LOAD_BASE):
 *     [0 .. 3]            __ce_api slot (kernel writes the table pointer)
 *     [4 .. 3+code)       code (g_ob)
 *     [4+code .. end)     data (g_data)
 * Functions were emitted at code offsets and every `call`/data/immediate
 * that names an address was recorded as a relocation in code coordinates;
 * here everything is shifted by 4 (the api slot) and made absolute.
 * ================================================================== */
#define IMG_MAX (256 * 1024)      /* CE_MAX_IMAGE */
static int      g_imglen;         /* total .ce file size, header included */
static uint8_t  g_hdr[20];

/* little-endian store into a byte buffer (no alignment requirement) */
static void put32le(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}

/* patch a dword inside g_ob (code buffer) */
static void code_patch(int at, uint32_t v) { ob_patch((uint32_t)at, v); }

/* the api slot address equals CE_LOAD_BASE + code + data (see fixup_code) */
static uint32_t g_api_slot_addr_code;

static void fixup_code(void) {
    /* The CE file is a 20-byte header followed by the raw body (code, data,
     * 4-byte api slot).  The kernel loads the BODY at CE_LOAD_BASE, so all
     * absolute addresses equal base + body offsets (no header shift).
     * rel32 jumps/calls inside the code buffer need no adjustment. */
    g_api_slot_addr_code = CE_LOAD_BASE + (uint32_t)g_olen + (uint32_t)g_dlen;
    uint32_t base = CE_LOAD_BASE;
    /* data relocations: mov eax, imm32 -> absolute address of data */
    for (int i = 0; i < g_nrel; i++)
        code_patch(g_rel_at[i], base + g_olen + (uint32_t)g_rel_off[i]);
    for (int i = 0; i < g_ncall; i++) {
        sym_t *f = &g_sym[g_call_sym[i]];
        int site = g_call_at[i];
        int rel = f->addr - (site + 4);
        code_patch(site, (uint32_t)rel);
    }
    /* api slot loads: absolute address of the slot itself */
    for (int i = 0; i < g_napiref; i++)
        code_patch(g_apiref[i], (uint32_t)g_api_slot_addr_code);
    /* entry stub: jmp ce_main (rel32 from the stub's next instruction) */
    if (g_entry_jmp >= 0)
        code_patch(g_entry_jmp, (uint32_t)(g_entry_off - (g_entry_jmp + 4)));
}



static int build_image(catos_api_t *api) {
    /* header (20 bytes): magic, entry, bss, api slot offset, image size */
    uint32_t img = (uint32_t)g_olen + (uint32_t)g_dlen + 4;   /* body + slot */
    g_imglen = 20 + (int)img;
    if (img > IMG_MAX) { (void)api; err_set("image too large"); return 0; }
    g_hdr[0] = (uint8_t)'C'; g_hdr[1] = (uint8_t)'E';
    g_hdr[2] = (uint8_t)'0'; g_hdr[3] = (uint8_t)'1';
    put32le(&g_hdr[4],  0);                    /* entry: stub at body offset 0 */
    put32le(&g_hdr[8],  0);                    /* bss (none)            */
    put32le(&g_hdr[12], (uint32_t)(g_olen + g_dlen));  /* api slot off   */
    put32le(&g_hdr[16], img);                  /* image size            */
    return 1;
}

/* Stream the .ce file to the volume: header, code, data, api slot.
 * api->file_write caps a single call at 16 KiB, so each piece is written
 * in chunks; mode 0 truncates on the first call and 1 appends after.
 * Writing straight from the code/data buffers avoids materialising a
 * second full copy of the image (see the memory-budget note above). */
static int save_image(catos_api_t *api, const char *out) {
    int mode = 0;                      /* first chunk truncates the file */
    int total = 0;

    /* 1. the 20-byte header */
    if (api->file_write(out, (const char *)g_hdr, 20, mode) < 0) goto werr;
    mode = 1; total += 20;

    /* 2. code */
    for (uint32_t off = 0; off < g_olen; ) {
        uint32_t n = g_olen - off;
        if (n > 16384u) n = 16384u;
        if (api->file_write(out, (const char *)(g_ob + off), (int)n, mode) < 0) goto werr;
        off += n; total += (int)n;
    }

    /* 3. data */
    for (uint32_t off = 0; off < g_dlen; ) {
        uint32_t n = g_dlen - off;
        if (n > 16384u) n = 16384u;
        if (api->file_write(out, (const char *)(g_data + off), (int)n, mode) < 0) goto werr;
        off += n; total += (int)n;
    }

    /* 4. the __ce_api slot (kernel fills it at load time) */
    {
        uint8_t zero[4] = { 0, 0, 0, 0 };
        if (api->file_write(out, (const char *)zero, 4, mode) < 0) goto werr;
        total += 4;
    }

    if (total != g_imglen) { api->write("cc: short write\n"); return 0; }
    return 1;

werr:
    api->write("cc: write failed on "); api->write(out); api->write("\n");
    return 0;
}

/* ====================================================================
 * entry
 * ================================================================== */
/* ce_main must sit at image offset 0 (see ce.ld / build_ce.sh) */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();

    api->write("CatOS cc -- tiny C compiler (target one)\n");
    api->write("--------------------------------------\n");
    if (argc < 3) {
        api->write("usage: run /bin/cc.ce SRC.C OUT.CE\n");
        return 1;
    }

    /* Take the working sets from the heap: the CE image+bss window is only
     * 512 KiB (USER_IMAGE_BASE..USER_VSYSCALL) and the code must fit too,
     * while api->alloc() draws from a separate 256 KiB window. */
    g_ob  = (uint8_t *)cc_alloc(api, OB_MAX,  g_ob_static,  &g_ob_cap);
    g_src = (char *)   cc_alloc(api, SRC_MAX, (void *)g_src_static, &g_src_cap);
    {
        uint32_t cap = 0;
        token_t *t = (token_t *)cc_alloc(api, (uint32_t)TOK_MAX * sizeof(token_t),
                                          g_tok_static, &cap);
        g_tok = t; g_tok_cap = cap / (int)sizeof(token_t);
    }
    api->write("heap: code "); api->print_int((int)g_ob_cap);
    api->write(" src "); api->print_int((int)g_src_cap);
    api->write(" tok "); api->print_int(g_tok_cap); api->write("\n");
    api->write("source: "); api->write(argv[1]); api->write("\n");
    /* api->file_read caps one call at 16 KiB, so read the source in
     * chunks until the buffer is full or the file ends. */
    int n = 0;
    for (;;) {
        int want = (int)g_src_cap - 1 - n;
        if (want <= 0) break;
        if (want > 16384) want = 16384;
        int got = api->file_read(argv[1], g_src + n, want, n);
        if (got < 0) { if (n > 0) break; n = got; break; }
        if (got == 0) break;
        n += got;
        if (got < want) break;
    }
    if (n < 0) {
        api->write("cc: cannot read source ("); api->print_int(n); api->write(")\n");
        return 1;
    }
    g_slen = (uint32_t)n;
    g_src[g_slen] = 0;

    if (!tokenize()) {
        api->write("cc: lexer: "); api->write(g_msg); api->write("\n");
        return 1;
    }
    api->write("  "); api->print_int(n); api->write(" bytes, ");
    api->print_int(g_ntok); api->write(" tokens\n");

    /* the api slot occupies image offset 4 (after nothing); code starts at
     * code offset 0 which maps to image offset 4 */
    /* The api slot is stored after the code and the data, so no code
     * byte is clobbered; its absolute address is CE_LOAD_BASE + code + data. */
    /* g_api_slot_addr_code is computed in fixup_code(), once code+data are final */

    parse_program();
    if (g_err) {
        api->write("cc: line "); api->print_int(g_tok[g_ti].line);
        api->write(": "); api->write(g_msg);
        api->write(" [token: ");
        if (g_tok[g_ti].kind == TK_ID) {
            char tmp[32];
            int n = g_tok[g_ti].len; if (n > 30) n = 30;
            for (int q = 0; q < n; q++) tmp[q] = g_tok[g_ti].sp[q];
            tmp[n] = 0;
            api->write("id '");
            api->write(tmp);
            api->write("'");
        } else if (g_tok[g_ti].kind == TK_PUNCT) {
            api->write("punct ");
            api->write(PUNCTS[g_tok[g_ti].op]);
        } else {
            api->write("kind ");
            api->print_int(g_tok[g_ti].kind);
        }
        api->write("]\n");
        return 1;
    }

    fixup_code();

    api->write("  code "); api->print_int((int)g_olen);
    api->write(" B, data "); api->print_int((int)g_dlen);
    api->print_int(0);
    api->write(" B\n");

    if (!build_image(api)) {
        api->write("cc: "); api->write(g_msg); api->write("\n");
        return 1;
    }
    api->write("  image "); api->print_int(g_imglen); api->write(" bytes\n");

    if (!save_image(api, argv[2])) return 1;

    api->write("OK -> "); api->write(argv[2]); api->write("\n");
    return 0;
}
