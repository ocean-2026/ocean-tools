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
    int  addr, nparams, stidx;   /* stidx: index into g_st when typed over a struct */
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

/* data-to-data relocations: global pointer slots that hold the absolute
 * address of a string inside the data section (patched in fixup_code,
 * once the final code size -- and therefore the data base -- is known) */
#define DREL_MAX 64
static int g_drel_at[DREL_MAX];
static int g_drel_off[DREL_MAX];
static int g_drel_n;

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

/* ---- struct definitions (stage 1) ---- */
#define STRUCT_MAX 16
#define SMEM_MAX   48
#define TAG_MAX    24
typedef struct {
    char tag[TAG_MAX];
    int size, nmemb;
    int m_off[SMEM_MAX];
    int m_sz[SMEM_MAX];     /* 1=char, 4=int/ptr */
    int m_ptr[SMEM_MAX];    /* member is a pointer */
    int m_arr[SMEM_MAX];    /* member is an array */
    int m_asz[SMEM_MAX];    /* array size when m_arr */
    char m_name[SMEM_MAX][16];
} stdef_t;
static stdef_t g_st[STRUCT_MAX];
static int g_nst;

static stdef_t *st_find(const char *n, int len) {
    for (int i = 0; i < g_nst; i++)
        if (name_eqn(g_st[i].tag, n, len)) return &g_st[i];
    return 0;
}
/* resolve a member by name; returns byte offset or -1; fills out params */
static int st_member(stdef_t *d, const char *n, int len,
                     int *osz, int *optr, int *oarr) {
    for (int i = 0; i < d->nmemb; i++)
        if (name_eqn(d->m_name[i], n, len)) {
            if (osz)  *osz  = d->m_sz[i];
            if (optr) *optr = d->m_ptr[i];
            if (oarr) *oarr = d->m_arr[i];
            return d->m_off[i];
        }
    return -1;
}

/* ---- typedef aliases (stage 1) ---- */
#define TALIAS_MAX 24
typedef struct {
    char name[TAG_MAX];
    int sz;        /* base size (1/4/struct-size) */
    int ptr;       /* is a pointer typedef */
    int stidx;     /* >=0 when aliases a struct type */
} alias_t;
static alias_t g_alias[TALIAS_MAX];
static int g_nalias;
static alias_t *al_find(const char *n, int len) {
    for (int i = 0; i < g_nalias; i++)
        if (name_eqn(g_alias[i].name, n, len)) return &g_alias[i];
    return 0;
}
static void add_alias(const char *sp, int len, int sz, int ptr, int stidx) {
    if (g_nalias >= TALIAS_MAX) { err_set("too many typedefs"); return; }
    alias_t *a = &g_alias[g_nalias++];
    unsigned i; for (i = 0; i < sizeof(*a); i++) ((unsigned char *)a)[i] = 0;
    if (len > TAG_MAX - 1) len = TAG_MAX - 1;
    sncpy(a->name, sp, len); a->name[len] = 0;
    a->sz = sz; a->ptr = ptr; a->stidx = stidx;
}

/* ---- enum constants (stage 1) ---- */
#define CONST_MAX 96
typedef struct { char name[24]; int val; } const_t;
static const_t g_const[CONST_MAX];
static int g_nconst;
static const_t *c_find(const char *n, int len) {
    for (int i = 0; i < g_nconst; i++)
        if (name_eqn(g_const[i].name, n, len)) return &g_const[i];
    return 0;
}

/* size used by an lvalue chain for store/load (set by gen_lvalue) */
static int g_lv_sz = 4;

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
static int parse_type(int *sz, int *is_ptr, int *stidx);
static void do_incdec(int kind, int prefix);

static sym_t *local_new(const char *n, int len, int sz, int is_ptr, int is_arr, int arr_sz, int stidx) {
    if (g_nloc >= LOC_MAX) { err_set("too many locals"); return 0; }
    sym_t *s = sym_new(n, len, SY_LVAR);
    if (!s) return 0;
    int unit = is_ptr ? 4 : sz;
    int bytes = is_arr ? arr_sz * unit : (is_ptr ? 4 : (sz > 4 ? sz : 4));
    if (bytes < 4) bytes = 4;
    g_frame += bytes;
    s->off = -g_frame;
    s->sz = sz;
    s->is_ptr = is_ptr;
    s->is_arr = is_arr;
    s->arr_sz = arr_sz;
    s->stidx = stidx;
    g_loc[g_nloc++] = s;
    return s;
}

/* helper: emit `mov eax, imm32` with a relocation to a data offset */
static void e_data_addr(int data_off) {
    ob8(0xB8); ob32(0);
    if (g_nrel < REL_MAX) { g_rel_at[g_nrel] = (int)ob_at() - 4; g_rel_off[g_nrel] = data_off; g_nrel++; }
    else err_set("too many relocs");
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

/* decode a string literal's raw source text (sp[0..len)) into dst, up to
 * max bytes; returns bytes written (no NUL appended -- callers terminate).
 * Shared by the local and the global string-initialiser paths so escapes
 * (\n \t \r \0 \\ \") decode identically everywhere. */
static int str_decode(char *dst, int max, const char *sp, int len) {
    int k = 0;
    for (int i = 0; i < len && k < max; i++) {
        char c = sp[i];
        if (c == '\\' && i + 1 < len) {
            char e = sp[++i];
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
        dst[k++] = c;
    }
    return k;
}

/* postfix expression: literal / name / call / index */
/* Emit the address of an lvalue (name already consumed).  Applies an optional
 * postfix chain of [index], .member and ->member; when do_load is set the
 * final scalar/pointer value is loaded from the resulting address (array
 * lvalues decay to their address instead).  g_lv_sz receives the element size
 * for a subsequent store. */
static void gen_lvalue(sym_t *s, int do_load) {
    int esize, obj_st, obj_ptr, obj_arr, obj_sz;
    if (s->kind == SY_LVAR) e_lea_eax_ebp(s->off);
    else                    e_data_addr(s->addr);

    if (s->is_arr) {
        obj_arr = 1;
        if (s->is_ptr)            { obj_ptr = 1; obj_st = s->stidx; esize = 4; }
        else if (s->stidx >= 0)   { obj_ptr = 0; obj_st = s->stidx; esize = s->sz; }
        else                      { obj_ptr = 0; obj_st = -1;        esize = s->sz; }
        obj_sz = s->sz;
    } else if (s->is_ptr) {
        obj_ptr = 1; obj_st = s->stidx; obj_arr = 0; obj_sz = 4; esize = 4;
    } else {
        obj_ptr = 0; obj_st = s->stidx; obj_arr = 0; obj_sz = s->sz; esize = s->sz;
    }

    for (;;) {
        if (at_op(OP_LSB)) {
            g_ti++;
            e_push_eax();
            gen_expr();
            if (esize != 1) { ob8(0x69); ob8(0xC0); ob32((uint32_t)esize); }
            e_pop_ecx();
            ob8(0x01); ob8(0xC1);              /* add ecx,eax */
            e_mov_eax_ecx();
            if (!accept_op(OP_RSB)) { err_set("expected ]"); return; }
            /* After indexing, the object is a single array element whose width
               is `esize` -- the unit we just scaled the index by.  Do NOT
               re-derive from the original symbol s->sz: when indexing a struct
               member array such as r.buf[1], s is the enclosing struct var and
               s->sz is the whole struct size, which would make a char element
               store a 4-byte mov and clobber the next field.  Keep obj_ptr /
               obj_st as they were entering this index (plain struct arrays keep
               their stidx so a following .member still resolves). */
            obj_sz = esize;
            obj_arr = 0;
            continue;
        }
        if (at_op(OP_DOT) || at_op(OP_ARROW)) {
            int arrow = at_op(OP_ARROW);
            g_ti++;
            token_t *mn = cur();
            if (mn->kind != TK_ID) { err_set("member name expected"); return; }
            if (obj_st < 0) { err_set("member access on non-struct"); g_ti++; return; }
            stdef_t *d = &g_st[obj_st];
            int msz = 4, mptr = 0, marr = 0;
            int moff = st_member(d, mn->sp, mn->len, &msz, &mptr, &marr);
            g_ti++;
            if (moff < 0) { err_set("no such member"); return; }
            if (arrow) e_ld_eax_ptr();          /* -> : deref the pointer slot */
            if (moff) { ob8(0x81); ob8(0xC0); ob32((uint32_t)moff); }  /* add eax,imm */
            if (mptr)      { obj_ptr = 1; obj_st = -1; obj_arr = 0; obj_sz = 4; esize = 4; }
            else if (marr) { obj_arr = 1; obj_ptr = 0; obj_st = -1; obj_sz = msz; esize = msz; }
            else           { obj_arr = 0; obj_ptr = 0; obj_st = -1; obj_sz = msz; esize = msz; }
            continue;
        }
        break;
    }

    g_lv_sz = obj_sz;
    if (at_op(OP_INC) || at_op(OP_DEC)) {       /* postfix ++/-- */
        int kind = at_op(OP_INC) ? OP_INC : OP_DEC;
        g_ti++;
        do_incdec(kind, 0);
        return;
    }
    if (do_load && !obj_arr) {
        if (obj_sz == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();
    }
}

static void gen_postfix(void) {
    token_t *t = cur();
    if (t->kind == TK_NUM || t->kind == TK_CHR) {
        e_mov_eax(t->num);
        g_ti++;
        return;
    }
    if (t->kind == TK_STR) {
        int o = (int)g_dlen;
        int w = str_decode((char *)&g_data[g_dlen], (int)sizeof(g_data) - 1 - (int)g_dlen,
                           t->sp, t->len);
        g_dlen += (uint32_t)w;
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
        else err_set("too many api calls");
        ob8(0xFF); ob8(0x50); ob8((uint32_t)(slot * 4));
        e_add_esp(n * 4);
        return;
    }

    sym_t *s = lfind(t->sp, t->len);
    if (!s) s = gfind(t->sp, t->len);
    if (!s) {
        const_t *c = c_find(t->sp, t->len);
        if (c) { e_mov_eax(c->val); g_ti++; return; }
        err_set("undeclared name");
        return;
    }
    g_ti++;

    if (s->kind == SY_FUNC) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int n = push_args();
        ob8(0xE8); ob32(0);
        if (g_ncall < CALLMAX) {
            g_call_at[g_ncall] = (int)ob_at() - 4;
            g_call_sym[g_ncall] = (int)(s - g_sym);
            g_ncall++;
        } else err_set("too many calls");
        e_add_esp(n * 4);
        return;
    }
    gen_lvalue(s, 1);
}

static void gen_unary(void) {
    if (at_kw("sizeof")) {
        g_ti++;
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        int deref = 0;
        if (accept_op(OP_STAR)) deref = 1;
        int sz = 4;
        token_t *tt = cur();
        if (tt->kind == TK_ID) {
            alias_t *a = al_find(tt->sp, tt->len);
            sym_t *vs = lfind(tt->sp, tt->len);
            if (!vs) vs = gfind(tt->sp, tt->len);
            if (a) {
                g_ti++;
                sz = (a->stidx >= 0) ? g_st[a->stidx].size : a->sz;
                if (deref) sz = 4;
            } else if (vs) {
                g_ti++;
                if (deref)      sz = (vs->stidx >= 0) ? g_st[vs->stidx].size : (vs->sz == 1 ? 1 : 4);
                else if (vs->is_arr) sz = vs->arr_sz * (vs->is_ptr ? 4 : vs->sz);
                else            sz = vs->sz;
            } else { err_set("sizeof expects type"); return; }
        } else {
            int tsz = 4, tp = 0, tst = -1;
            if (!parse_type(&tsz, &tp, &tst)) { err_set("sizeof expects type"); return; }
            sz = (tst >= 0) ? g_st[tst].size : tsz;
            if (deref) sz = 4;
        }
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_mov_eax(sz);
        return;
    }
    int o = tok_op();
    if (g_tok[g_ti].kind == TK_PUNCT) {
        if (o == OP_NOT)   { g_ti++; gen_unary(); e_test_setcc(0x4); return; }
        if (o == OP_MINUS) { g_ti++; gen_unary(); e_neg_eax(); return; }
        if (o == OP_TILDE) { g_ti++; gen_unary(); e_not_eax(); return; }
        if (o == OP_AMP)   {
            g_ti++;
            token_t *a = cur();
            if (a->kind == TK_ID) {
                sym_t *as = lfind(a->sp, a->len);
                if (!as) as = gfind(a->sp, a->len);
                if (as && as->kind != SY_FUNC) { g_ti++; gen_lvalue(as, 0); return; }
            }
            gen_unary();
            return;
        }   /* address-of */
        if (o == OP_STAR)  { g_ti++; gen_unary(); e_ld_eax_ptr(); return; }
        if (o == OP_PLUS)  { g_ti++; gen_unary(); return; }
        if (o == OP_INC || o == OP_DEC) {       /* prefix ++/-- */
            g_ti++;
            token_t *op = cur();
            if (op->kind == TK_ID) {
                sym_t *vs = lfind(op->sp, op->len);
                if (!vs) vs = gfind(op->sp, op->len);
                if (vs && vs->kind != SY_FUNC) {
                    g_ti++;
                    gen_lvalue(vs, 0);
                    do_incdec(o, 1);
                    return;
                }
            }
            err_set("bad prefix ++");
            return;
        }
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
        case OP_SHL:     ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xE0); break; /* xchg eax,ecx; shl eax,cl */
        case OP_SHR:     ob8(0x87); ob8(0xC8); ob8(0xD3); ob8(0xF8); break; /* xchg eax,ecx; sar eax,cl */
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

/* assignment: `lvalue OP= rhs`; lvalue may carry [index] / .member / ->member */
static void gen_assign_expr(void) {
    token_t *t = cur();
    if (t->kind == TK_ID) {
        /* look past an optional postfix chain to find the assignment operator */
        int la = 1;
        for (;;) {
            token_t *x = peek(la);
            if (x->kind == TK_PUNCT && x->op == OP_LSB) {
                int depth = 1; la++;
                while (peek(la)->kind != TK_EOF && depth) {
                    if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_LSB) depth++;
                    else if (peek(la)->kind == TK_PUNCT && peek(la)->op == OP_RSB) depth--;
                    la++;
                }
            } else if (x->kind == TK_PUNCT && (x->op == OP_DOT || x->op == OP_ARROW)) {
                la++;
                if (peek(la)->kind == TK_ID) la++;
            } else break;
        }
        token_t *nx = peek(la);
        if (nx->kind == TK_PUNCT) {
            int o = nx->op;
            if (o == OP_ASG || o == OP_PLUSASG || o == OP_MINUSASG ||
                o == OP_MULASG || o == OP_DIVASG || o == OP_MODASG) {
                sym_t *s = lfind(t->sp, t->len);
                if (!s) s = gfind(t->sp, t->len);
                if (s && s->kind != SY_FUNC) {
                    g_ti++;                       /* consume the NAME */
                    gen_lvalue(s, 0);            /* eax = lvalue address */
                    int es = g_lv_sz;
                    e_push_eax();                 /* keep lvalue address */
                    if (o == OP_ASG) {
                        g_ti++;
                        gen_assign_expr();        /* rhs -> eax */
                        e_mov_ecx_eax();          /* ecx = value */
                        e_pop_eax();              /* eax = address */
                        if (es == 1) e_st8_eax_ptr(); else e_st_eax_ptr();
                        e_mov_eax_ecx();          /* result = value */
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

/* goto labels (stage 3) */
#define LBL_MAX 48
static char g_lbl_name[LBL_MAX][24];
static int  g_lbl_off[LBL_MAX];      /* code offset of definition; -1 if forward */
static int  g_nlbl;
static int  g_goto_at[LBL_MAX];      /* unresolved forward goto jump site */
static int  g_goto_lbl[LBL_MAX];
static int  g_ngoto;
static int  lbl_decl(const char *sp, int len) {
    for (int i = 0; i < g_nlbl; i++)
        if (name_eqn(g_lbl_name[i], sp, len)) return i;
    if (g_nlbl >= LBL_MAX) { err_set("too many labels"); return 0; }
    int idx = g_nlbl++;
    unsigned k; for (k = 0; k < sizeof(g_lbl_name[idx]); k++) g_lbl_name[idx][k] = 0;
    int L = len > 23 ? 23 : len;
    sncpy(g_lbl_name[idx], sp, L); g_lbl_off[idx] = -1;
    return idx;
}

/* increment/decrement an lvalue whose address is in EAX; uses g_lv_sz.
 * kind=OP_INC/OP_DEC; prefix=1 gives new value, 0 gives old value. */
static void do_incdec(int kind, int prefix) {
    int es = g_lv_sz;
    e_push_eax();                              /* [esp]=addr */
    ob8(0x8B); ob8(0x04); ob8(0x24);           /* eax=[esp]=addr */
    if (es == 1) e_ldsx_eax_ptr(); else e_ld_eax_ptr();   /* eax=old */
    e_push_eax();                              /* [esp]=addr,[esp+4]=old */
    if (kind == OP_INC) { ob8(0x83); ob8(0xC0); ob8(1); } /* add eax,1 */
    else                { ob8(0x83); ob8(0xE8); ob8(1); } /* sub eax,1 */
    ob8(0x8B); ob8(0x4C); ob8(0x24); ob8(4);  /* ecx=[esp+4]=addr */
    if (es == 1) { ob8(0x88); ob8(0x01); }    /* mov [ecx],al */
    else         { ob8(0x89); ob8(0x01); }    /* mov [ecx],eax */
    if (prefix) {
        /* result already in eax (new) */
    } else {
        ob8(0x8B); ob8(0x04); ob8(0x24);      /* eax=[esp]=old */
    }
    e_add_esp(8);
}

static int is_type_kw(void) {
    if (at_kw("int") || at_kw("char") || at_kw("void") || at_kw("unsigned") ||
        at_kw("struct")) return 1;
    token_t *t = cur();
    if (t->kind == TK_ID) {
        for (int i = 0; i < g_nalias; i++)
            if (name_eqn(g_alias[i].name, t->sp, t->len)) return 1;
    }
    return 0;
}

/* parse a type.  *stidx receives the struct index (>=0) when the type is a
 * struct or a typedef over a struct; pass NULL when unneeded. */
static int parse_type(int *sz, int *is_ptr, int *stidx) {
    *sz = 4; *is_ptr = 0;
    if (stidx) *stidx = -1;
    if (accept_kw("char")) *sz = 1;
    else if (accept_kw("int")) *sz = 4;
    else if (accept_kw("unsigned")) { *sz = 4; accept_kw("int"); }
    else if (accept_kw("void")) *sz = 4;
    else if (accept_kw("struct")) {
        token_t *t = cur();
        if (t->kind != TK_ID) { err_set("struct tag expected"); return 0; }
        stdef_t *d = st_find(t->sp, t->len);
        if (!d) { err_set("undefined struct"); return 0; }
        g_ti++;
        *sz = d->size;
        if (stidx) *stidx = (int)(d - g_st);
    }
    else {
        token_t *t = cur();
        if (t->kind != TK_ID) return 0;
        alias_t *a = al_find(t->sp, t->len);
        if (!a) return 0;
        g_ti++;
        *sz = a->sz;
        if (stidx) *stidx = a->stidx;
    }
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
    int sz, ptr, st = -1, is_arr = 0, arr_sz = 0;
    if (!parse_type(&sz, &ptr, &st)) { err_set("type expected"); return; }
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
        sym_t *s = local_new(nm->sp, nm->len, sz, ptr, is_arr, arr_sz, st);
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
        /* body -> step; with no step clause the body loops straight back
         * to the condition (top) so the condition is re-evaluated */
        e_jmp_back(has_step ? step_pos : top);
        if (jz >= 0) e_patch_here(jz);
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        /* continue must jump to the step (or to the condition when there
         * is no step clause); leaving the E9 placeholder unpatched made
         * `continue` fall through in the body and skip the step */
        for (int i = cb; i < g_ncont; i++)
            ob_patch((uint32_t)g_cont[i], (uint32_t)((has_step ? step_pos : top) - g_cont[i] - 4));
        g_ncont = cb;
        return;
    }
    if (accept_kw("switch")) {
        if (!accept_op(OP_LP)) { err_set("expected ("); return; }
        gen_expr();
        if (!accept_op(OP_RP)) { err_set("expected )"); return; }
        e_push_eax();                          /* save switch value */
        if (!accept_op(OP_LB)) { err_set("expected {"); return; }
        int bb = g_nbrk;
        /* emit statements until break / next label / '}' */
        for (;;) {
            if (at_op(OP_RB) || cur()->kind == TK_EOF) break;
            if (accept_op(OP_SEMI)) continue;
            if (accept_kw("break")) {
                if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                continue;                     /* break out of case body */
            }
            if (accept_kw("default")) {
                if (!accept_op(OP_COLON)) { err_set("expected :"); return; }
                /* default body: run statements until next label / rb */
                while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
                    if (accept_op(OP_SEMI)) continue;
                    if (accept_kw("case") || accept_kw("default")) { err_set("stray label"); return; }
                    if (accept_kw("break")) {
                        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                        break;
                    }
                    gen_stmt();
                }
                continue;
            }
            if (accept_kw("case")) {
                int v = 0;
                if (accept_op(OP_MINUS)) v = -1;
                if (cur()->kind == TK_NUM || cur()->kind == TK_CHR) {
                    int base = cur()->num; g_ti++;
                    v = (v == -1) ? -base : base;
                } else {
                    const_t *cf = c_find(cur()->sp, cur()->len);
                    if (cf) { v = cf->val; g_ti++; }
                    else { err_set("case constant expected"); return; }
                }
                if (!accept_op(OP_COLON)) { err_set("expected :"); return; }
                ob8(0x8B); ob8(0x04); ob8(0x24);    /* mov eax,[esp] */
                ob8(0x3D); ob32((uint32_t)v);        /* cmp eax,imm32 */
                int jne = e_jnz_fwd();
                /* case body: statements until break / next label / rb */
                while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
                    if (accept_op(OP_SEMI)) continue;
                    if (at_kw("case") || at_kw("default")) break;
                    if (accept_kw("break")) {
                        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
                        if (g_nbrk < BRK_MAX) g_brk[g_nbrk++] = e_jmp_fwd();
                        break;
                    }
                    gen_stmt();
                }
                e_patch_here(jne);                   /* next case */
                continue;
            }
            gen_stmt();
        }
        if (!accept_op(OP_RB)) { err_set("expected }"); return; }
        for (int i = bb; i < g_nbrk; i++) e_patch_here(g_brk[i]);
        g_nbrk = bb;
        e_add_esp(4);                          /* pop switch value */
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
    if (accept_kw("goto")) {
        token_t *ln = cur();
        if (ln->kind != TK_ID) { err_set("label expected"); return; }
        g_ti++;
        if (!accept_op(OP_SEMI)) { err_set("expected ;"); return; }
        int idx = lbl_decl(ln->sp, ln->len);
        if (g_lbl_off[idx] >= 0) {
            e_jmp_back(g_lbl_off[idx]);
        } else {
            int at = e_jmp_fwd();
            if (g_ngoto < LBL_MAX) { g_goto_at[g_ngoto] = at; g_goto_lbl[g_ngoto] = idx; g_ngoto++; }
            else err_set("too many gotos");
        }
        return;
    }
    /* label:   (identifier followed by ':') */
    if (cur()->kind == TK_ID && peek(1)->kind == TK_PUNCT && peek(1)->op == OP_COLON) {
        token_t *ln = cur();
        int idx = lbl_decl(ln->sp, ln->len);
        g_ti += 2;
        g_lbl_off[idx] = (int)ob_at();
        for (int i = 0; i < g_ngoto; i++)
            if (g_goto_lbl[i] == idx && g_goto_at[i] >= 0) {
                ob_patch((uint32_t)g_goto_at[i],
                         (uint32_t)(ob_at() - g_goto_at[i] - 4));
                g_goto_at[i] = -1;
            }
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
            int psz = 4, pp = 0, pst = -1;
            if (!parse_type(&psz, &pp, &pst)) { accept_kw("int"); }
            token_t *pn = cur();
            if (pn->kind != TK_ID) { err_set("parameter expected"); return; }
            g_ti++;
            sym_t *ps = local_new(pn->sp, pn->len, psz, pp, 0, 0, pst);
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
    if (!at_op(OP_LB)) { err_set("expected {"); return; }
    gen_stmt();
    e_mov_eax(0);
    ob8(0x89); ob8(0xEC); ob8(0x5D); ob8(0xC3);
    ob_patch(sub_at + 2, (uint32_t)((g_frame + 15) & ~15u));
}

/* ---- stage 1: struct / typedef / enum top-level parsers ---- */

/* parse a struct body `struct [tag] { members }`; leaves cursor after '}'.
 * Used both for plain struct definitions and by typedef. */
static stdef_t *parse_struct_def(void) {
    accept_kw("struct");
    token_t *tag = cur();
    stdef_t *d = 0;
    int have_tag = 0;
    if (tag->kind == TK_ID) {
        g_ti++; have_tag = 1;
        d = st_find(tag->sp, tag->len);
        if (!d) {
            if (g_nst >= STRUCT_MAX) { err_set("too many structs"); return 0; }
            d = &g_st[g_nst++];
            unsigned i; for (i = 0; i < sizeof(*d); i++) ((unsigned char *)d)[i] = 0;
            int L = tag->len > TAG_MAX - 1 ? TAG_MAX - 1 : tag->len;
            sncpy(d->tag, tag->sp, L); d->tag[L] = 0;
        }
    }
    if (!at_op(OP_LB)) {
        /* `struct tag;` forward declaration with no body */
        if (!accept_op(OP_SEMI)) err_set("expected ;");
        return 0;
    }
    if (!d) {
        if (g_nst >= STRUCT_MAX) { err_set("too many structs"); return 0; }
        d = &g_st[g_nst++];
        unsigned i; for (i = 0; i < sizeof(*d); i++) ((unsigned char *)d)[i] = 0;
    }
    g_ti++;                                   /* '{' */
    int off = 0;
    d->nmemb = 0;
    while (!at_op(OP_RB) && cur()->kind != TK_EOF && !g_err) {
        if (accept_op(OP_SEMI)) continue;
        int msz = 4, mptr = 0, mst = -1;
        if (!parse_type(&msz, &mptr, &mst)) { err_set("member type expected"); return 0; }
        token_t *nm = cur();
        if (nm->kind != TK_ID) { err_set("member name expected"); return 0; }
        g_ti++;
        int is_arr = 0, asz = 0;
        if (accept_op(OP_LSB)) {
            asz = const_size(); if (!asz) asz = 16;
            if (!accept_op(OP_RSB)) err_set("expected ]");
            is_arr = 1;
        }
        int align = mptr ? 4 : msz;
        if (align != 1) off = (off + align - 1) & ~(align - 1);
        int idx = d->nmemb;
        int L = nm->len > 15 ? 15 : nm->len;
        sncpy(d->m_name[idx], nm->sp, L); d->m_name[idx][L] = 0;
        d->m_off[idx] = off;
        d->m_sz[idx] = msz;
        d->m_ptr[idx] = mptr;
        d->m_arr[idx] = is_arr;
        d->m_asz[idx] = asz;
        int unit = mptr ? 4 : msz;
        off += is_arr ? asz * unit : unit;
        d->nmemb++;
        if (!accept_op(OP_SEMI)) { err_set("expected ; in struct"); return 0; }
    }
    if (!accept_op(OP_RB)) { err_set("expected }"); return 0; }
    d->size = (off + 3) & ~3;
    return d;
}

static void parse_typedef(void) {
    accept_kw("typedef");
    if (at_kw("struct")) {
        stdef_t *d = parse_struct_def();
        token_t *nm = cur();
        if (nm->kind == TK_ID) {
            g_ti++;
            int st = d ? (int)(d - g_st) : -1;
            add_alias(nm->sp, nm->len, d ? d->size : 4, 0, st);
        }
        if (!accept_op(OP_SEMI)) err_set("expected ;");
        return;
    }
    int sz = 4, ptr = 0, st = -1;
    if (!parse_type(&sz, &ptr, &st)) { err_set("typedef type expected"); return; }
    token_t *nm = cur();
    if (nm->kind != TK_ID) { err_set("typedef name expected"); return; }
    g_ti++;
    while (accept_op(OP_STAR)) ptr = 1;
    add_alias(nm->sp, nm->len, sz, ptr, st);
    if (!accept_op(OP_SEMI)) err_set("expected ;");
}

static void parse_enum(void) {
    accept_kw("enum");
    if (cur()->kind == TK_ID) g_ti++;          /* tag ignored */
    if (accept_op(OP_LB)) {
        int v = 0;
        for (;;) {
            token_t *nm = cur();
            if (nm->kind != TK_ID) { err_set("enum name expected"); return; }
            g_ti++;
            if (accept_op(OP_ASG)) {
                int neg = 0;
                if (accept_op(OP_MINUS)) neg = 1;
                if (cur()->kind != TK_NUM) { err_set("enum value"); return; }
                v = cur()->num; g_ti++;
                if (neg) v = -v;
            }
            if (g_nconst < CONST_MAX) {
                int L = nm->len > 23 ? 23 : nm->len;
                unsigned i;
                for (i = 0; i < sizeof(g_const[g_nconst].name); i++) g_const[g_nconst].name[i] = 0;
                sncpy(g_const[g_nconst].name, nm->sp, L);
                g_const[g_nconst].val = v;
                g_nconst++;
            } else err_set("too many enum constants");
            v++;
            if (accept_op(OP_COMMA)) continue;
            break;
        }
        if (!accept_op(OP_RB)) { err_set("expected }"); return; }
    }
    if (!accept_op(OP_SEMI)) err_set("expected ;");
}

static void parse_program(void) {
    /* The kernel's CE loader demands entry == 0 and api_off + 4 <= image_size.
     * ce_main may be defined anywhere in the source, so we plant a 5-byte
     * `jmp ce_main` stub at code offset 0 (= image offset 0 after the 20-byte
     * header); the header entry stays 0 and the stub is patched in fixup_code. */
    g_entry_jmp = e_jmp_fwd();
    while (cur()->kind != TK_EOF && !g_err) {
        if (accept_op(OP_SEMI)) continue;
        if (accept_kw("typedef")) { parse_typedef(); continue; }
        if (at_kw("enum")) { parse_enum(); continue; }
        if (at_kw("struct")) {
            int li = 1, isdef = 0;
            if (peek(li)->kind == TK_ID) li++;
            if (peek(li)->kind == TK_PUNCT && peek(li)->op == OP_LB) isdef = 1;
            if (isdef) {
                parse_struct_def();
                if (!accept_op(OP_SEMI)) err_set("expected ; after struct");
                continue;
            }
            /* otherwise: `struct tag v;` falls through as a normal decl */
        }
        int sz, ptr, st = -1;
        int is_func = 0;
        if (!parse_type(&sz, &ptr, &st)) {
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
            g->stidx = st;
            g->addr = (int)g_dlen;
            {
                int unit = ptr ? 4 : sz;
                int bytes = is_arr ? arr_sz * unit : (ptr ? 4 : (sz > 4 ? sz : 4));
                if (bytes < 4) bytes = 4;
                for (int i = 0; i < bytes && g_dlen < sizeof(g_data); i++) g_data[g_dlen++] = 0;
            }
            if (accept_op(OP_ASG)) {
                if (cur()->kind == TK_STR) {
                    token_t *st = cur();
                    if (is_arr) {
                        /* FIND-03: a global char array initialised from a
                         * string inlines the decoded bytes into its own
                         * reserved slot (NUL-terminated when it fits); the
                         * old pointer-style init left garbage in the array. */
                        int cap = arr_sz * (ptr ? 4 : sz);
                        if (cap < 4) cap = 4;
                        int w = str_decode((char *)&g_data[g->addr], cap,
                                           st->sp, st->len);
                        if (w < cap) g_data[g->addr + (uint32_t)w] = 0;
                        g_ti++;
                    } else {
                        /* char *p = "str": the string is appended at the
                         * end of the data section and the slot stores its
                         * offset (shared escape decoding with locals) */
                        int o = (int)g_dlen;
                        int w = str_decode((char *)&g_data[g_dlen],
                                           (int)sizeof(g_data) - 1 - (int)g_dlen,
                                           st->sp, st->len);
                        g_dlen += (uint32_t)w;
                        if (g_dlen < sizeof(g_data)) g_data[g_dlen++] = 0;
                        g_ti++;
                        if (g_drel_n < DREL_MAX) {
                            g_drel_at[g_drel_n] = (int)g->addr;
                            g_drel_off[g_drel_n] = o;
                            g_drel_n++;
                        } else err_set("too many data relocs");
                    }
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
    /* FIND-01: with no ce_main the entry stub would be patched to jump to
     * itself (rel32 = 0-(1+4) = -5, E9 FB FF FF FF) and hang the task;
     * reject the source instead of emitting a dead image. */
    if (!g_err && g_entry_off == 0) err_set("no ce_main");
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
    /* data-to-data relocations: global pointer slots -> absolute address
     * of their string in the data section (base + code + data offset) */
    for (int i = 0; i < g_drel_n; i++) {
        uint32_t v = base + g_olen + (uint32_t)g_drel_off[i];
        uint32_t a = (uint32_t)g_drel_at[i];
        g_data[a]     = (uint8_t)v;
        g_data[a + 1] = (uint8_t)(v >> 8);
        g_data[a + 2] = (uint8_t)(v >> 16);
        g_data[a + 3] = (uint8_t)(v >> 24);
    }
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
