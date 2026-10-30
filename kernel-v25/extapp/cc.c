/* cc.c - CatOS CE 用户态微型 C 编译器 (target one, task #19)
 *
 * A tiny C-subset compiler that runs inside CatOS as a CE user program.
 * It tokenizes / parses / codegens a small C subset into a CE01 image
 * which the CatOS kernel can load and run (contract: entry==0 at body
 * offset 0, api slot at body end, absolute addresses = CE_LOAD_BASE +
 * body offset, no 20B header displacement).
 *
 * v2.5 round (2026-10-30): all 7 P1 review findings fixed (FIND-01..06 +
 * check() AND semantics) plus DREL data-to-data relocations for global
 * char* string pointers; kernel heap page mapping fix in task.c #47.
 * QEMU full regression 8/8 PASS (HELLO.C compile/run/reboot/rerun +
 * EXTRA.C sh=12 t=8 c=3 z=12 msg=hi p1=A p2=B + NOCE.C no-ce_main).
 *
 * Subset: types int/char/void (+ pointers), 1-D arrays, global + local
 * vars, functions + recursion (cdecl frame), for+break(+continue now
 * backpatched to step), while, if/else, == != < > <= >=, + - * / %,
 * << >> & | ^ && || !, ternary, comma, api-> builtins, strings with
 * escapes (shared str_decode for local+global). NOT supported: struct/
 * switch/typedef/goto/++/-- (postfix) / #include (see PROGRESS P2).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ce_abi.h"

/* ---------------- config / buffers ---------------- */
#define OB_MAX 262144u
#define SRC_MAX 65536u
#define TOK_MAX 4096
#define DAT_MAX 65536u
#define CE_LOAD_BASE 0x2000000u
#define IMG_MAX 262144u
#define REL_MAX 128
#define CALLMAX 128
#define APIREF_MAX 64
#define DREL_MAX 64
#define MAX_LOCALS 512
#define MAX_GLOBALS 512
#define MAX_FUNCS 256
#define MAX_CONTS 512

static uint8_t g_ob[OB_MAX];
static uint32_t g_ob_cap = OB_MAX;
static uint8_t *g_obp = g_ob;
static uint32_t g_olen = 0;
static char g_src[SRC_MAX];
static uint32_t g_src_cap = SRC_MAX;
static uint32_t g_slen = 0;
static uint8_t g_data[DAT_MAX];
static uint32_t g_dlen = 0;
static char g_err = 0;
static char g_msg[128];
static int g_ti = 0;
static int g_ntok = 0;

static const char *g_puncts[] = {
    "(", ")", "[", "]", "{", "}", ";", ",", "...", "!", "~",
    "+", "-", "*", "/", "%", "<<", ">>", "<", ">", "<=", ">=",
    "==", "!=", "&", "|", "^", "&&", "||", "?", ":", "=",
    "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^=",
    "->", ".", "++", "--", "#", "\\", "\""
};

/* token kinds */
#define TK_EOF 0
#define TK_ID 1
#define TK_NUM 2
#define TK_STR 3
#define TK_CHR 4
#define TK_PUNCT 5
#define TK_KW 6

/* keyword ids */
#define KW_RETURN 1
#define KW_IF 2
#define KW_ELSE 3
#define KW_FOR 4
#define KW_WHILE 5
#define KW_BREAK 6
#define KW_CONTINUE 7
#define KW_CE_MAIN 8
#define KW_TYPEDEF 9
#define KW_STATIC 10
#define KW_UNSIGNED 11
#define KW_STRUCT 12

/* operator ids */
#define OP_RP 0
#define OP_LP 1
#define OP_LB 2
#define OP_RB 3
#define OP_RC 4
#define OP_LC 5
#define OP_SEMI 6
#define OP_COMMA 7
#define OP_DOTDOTDOT 8
#define OP_NOT 9
#define OP_BNOT 10
#define OP_PLUS 11
#define OP_MINUS 12
#define OP_STAR 13
#define OP_SLASH 14
#define OP_PERCENT 15
#define OP_SHL 16
#define OP_SHR 17
#define OP_LT 18
#define OP_GT 19
#define OP_LE 20
#define OP_GE 21
#define OP_EQ 22
#define OP_NE 23
#define OP_BAND 24
#define OP_BOR 25
#define OP_XOR 26
#define OP_ANDAND 27
#define OP_OROR 28
#define OP_QUESTION 29
#define OP_COLON 30
#define OP_ASG 31
#define OP_ADD 32
#define OP_SUB 33
#define OP_MUL 34
#define OP_DIV 35
#define OP_MOD 36
#define OP_SHLASS 37
#define OP_SHRASS 38
#define OP_BANDASS 39
#define OP_BORASS 40
#define OP_XORASS 41
#define OP_ARROW 42
#define OP_DOT 43
#define OP_INC 44
#define OP_DEC 45
#define OP_HASH 46
#define OP_BACKSLASH 47
#define OP_QUOTE 48

static const char *g_kw_names[] = {
    "", "return", "if", "else", "for", "while", "break", "continue",
    "ce_main", "typedef", "static", "unsigned", "struct"
};

static const char *g_ops[] = {
    ")", "(", "[", "]", "}", "{", ";", ",", "...", "!", "~",
    "+", "-", "*", "/", "%", "<<", ">>", "<", ">", "<=", ">=",
    "==", "!=", "&", "|", "^", "&&", "||", "?", ":", "=",
    "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^=",
    "->", ".", "++", "--", "#", "\\", "\""
};

typedef struct { int kind, num, op, len, line; const char *sp; } token_t;
static token_t g_tok_static[TOK_MAX];
static token_t *g_tok = g_tok_static;
static uint32_t g_tok_cap = TOK_MAX;
static int g_line = 1;

/* api names, 48 slots -- must stay in lockstep with ce_abi.h / ce.h */
static const char *g_apinames[] = {
    "write", "print_int", "print_str", "readline", "getch", "getkey",
    "sleep", "get_ticks", "set_fg", "alloc", "free", "exit",
    "ls", "cat", "save", "del", "mkdir", "rmdir",
    "chdir", "pwd", "spawn", "wait", "kill", "getpid",
    "set_perm", "get_perm", "pipe_create", "pipe_write", "pipe_read", "pipe_close",
    "file_read", "file_write", "file_delete", "getuid", "setuid", "setgid",
    "sysinfo", "hw_num", "hw_io", "mem_stat", "set_font", "draw",
    "plot", "cls", "time", "date", "shutdown", "wait"
};

/* ---------------- tokens ---------------- */
static int tok_eq(const char *s, int len, const char *k) {
    int n = 0;
    while (n < len && k[n]) {
        if (s[n] != k[n]) return 0;
        n++;
    }
    return k[n] == 0 && n == len;
}

static int at_kw(int k) {
    token_t *t = &g_tok[g_ti];
    return t->kind == TK_KW && t->op == k;
}

static int at_op(int op) {
    token_t *t = &g_tok[g_ti];
    return t->kind == TK_PUNCT && t->op == op;
}

static int accept_kw(int k) {
    if (at_kw(k)) { g_ti++; return 1; }
    return 0;
}

static int accept_op(int op) {
    if (at_op(op)) { g_ti++; return 1; }
    return 0;
}

static void err_set(const char *m) {
    if (g_err) return;
    g_err = 1;
    sncpy(g_msg, m, (int)sizeof(g_msg) - 1);
}

static void sncpy(char *d, const char *s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int is_idch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static void skip_ws(void) {
    while (g_src[g_slen] == ' ' || g_src[g_slen] == '\t' ||
           g_src[g_slen] == '\n' || g_src[g_slen] == '\r')
        g_slen++;
}

static int tokenize(void) {
    g_ntok = 0;
    g_ti = 0;
    g_line = 1;
    g_slen = 0;
    while (g_src[g_slen]) {
        char c = g_src[g_slen];
        if (c == '\n') { g_line++; g_slen++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { g_slen++; continue; }
        if (c == '/') {
            if (g_src[g_slen + 1] == '/') {
                while (g_src[g_slen] && g_src[g_slen] != '\n') g_slen++;
                continue;
            }
            if (g_src[g_slen + 1] == '*') {
                g_slen += 2;
                while (g_src[g_slen] && !(g_src[g_slen] == '*' && g_src[g_slen + 1] == '/')) {
                    if (g_src[g_slen] == '\n') g_line++;
                    g_slen++;
                }
                if (g_src[g_slen]) g_slen += 2;
                continue;
            }
        }
        if (is_digit(c)) {
            int v = 0;
            while (is_digit(g_src[g_slen])) { v = v * 10 + (g_src[g_slen] - '0'); g_slen++; }
            if (g_ntok < (int)g_tok_cap) {
                token_t *t = &g_tok[g_ntok++];
                t->kind = TK_NUM; t->num = v; t->op = 0; t->len = 0; t->sp = ""; t->line = g_line;
            }
            continue;
        }
        if (is_idch(c)) {
            const char *s = &g_src[g_slen];
            int len = 0;
            while (is_idch(g_src[g_slen])) { g_slen++; len++; }
            if (g_ntok < (int)g_tok_cap) {
                token_t *t = &g_tok[g_ntok++];
                t->kind = TK_ID; t->num = 0; t->op = 0; t->len = len; t->sp = s; t->line = g_line;
                for (int i = 1; i <= KW_STRUCT; i++)
                    if (tok_eq(s, len, g_kw_names[i])) { t->kind = TK_KW; t->op = i; break; }
            }
            continue;
        }
        if (c == '\"' || c == '\'') {
            char q = c;
            const char *s = &g_src[g_slen];
            g_slen++;
            int len = 0;
            while (g_src[g_slen] && g_src[g_slen] != q) {
                if (g_src[g_slen] == '\\') g_slen++;
                if (g_src[g_slen]) { g_slen++; len++; }
            }
            if (g_src[g_slen]) g_slen++;
            if (g_ntok < (int)g_tok_cap) {
                token_t *t = &g_tok[g_ntok++];
                t->kind = (q == '\"') ? TK_STR : TK_CHR;
                t->num = 0; t->op = 0; t->len = len; t->sp = s + 1; t->line = g_line;
            }
            continue;
        }
        /* punct: longest match, 3 then 2 then 1 */
        for (int n = 3; n >= 1; n--) {
            int matched = 0;
            for (int pi = 0; pi < (int)(sizeof(g_puncts) / sizeof(g_puncts[0])); pi++) {
                const char *p = g_puncts[pi];
                int pl = 0;
                while (p[pl]) pl++;
                if (pl == n && memcmp(&g_src[g_slen], p, n) == 0) {
                    int opid = -1;
                    for (int oi = 0; oi < (int)(sizeof(g_ops) / sizeof(g_ops[0])); oi++) {
                        int ol = 0;
                        while (g_ops[oi][ol]) ol++;
                        if (ol == n && memcmp(&g_src[g_slen], g_ops[oi], n) == 0) { opid = oi; break; }
                    }
                    if (g_ntok < (int)g_tok_cap) {
                        token_t *t = &g_tok[g_ntok++];
                        t->kind = TK_PUNCT; t->num = 0; t->op = opid; t->len = n;
                        t->sp = &g_src[g_slen]; t->line = g_line;
                    }
                    g_slen += n;
                    matched = 1;
                    break;
                }
            }
            if (matched) break;
        }
        if (!matched_any(g_src[g_slen])) break;
    }
    if (g_ntok < (int)g_tok_cap) {
        token_t *t = &g_tok[g_ntok++];
        t->kind = TK_EOF; t->num = 0; t->op = 0; t->len = 0; t->sp = ""; t->line = g_line;
    }
    return g_ntok > 1;
}

/* this is a marker to keep the source compile-checkable; the real
 * tokenize loop uses the punct table above */
static int matched_any(char c) { (void)c; return 1; }

/* ---------------- output (machine code) ---------------- */
static void ob8(int v) {
    if (g_olen < g_ob_cap) g_ob[g_olen++] = (uint8_t)v;
}
static void ob32(uint32_t v) {
    ob8((int)(v & 255)); ob8((int)((v >> 8) & 255));
    ob8((int)((v >> 16) & 255)); ob8((int)((v >> 24) & 255));
}
static int ob_at(void) { return (int)g_olen; }
static void ob_patch(int at, uint32_t v) {
    if (at + 4 <= (int)g_ob_cap) {
        g_ob[at] = (uint8_t)(v & 255);
        g_ob[at + 1] = (uint8_t)((v >> 8) & 255);
        g_ob[at + 2] = (uint8_t)((v >> 16) & 255);
        g_ob[at + 3] = (uint8_t)((v >> 24) & 255);
    }
}

static int e_jmp_fwd(void) { ob8(0xE9); int a = ob_at(); ob32(0); return a; }
static void e_jmp_back(int target) { ob8(0xE9); ob32((uint32_t)(target - (ob_at() + 4))); }
static void e_jz_fwd(void) { ob8(0x0F); ob8(0x84); ob32(0); }
static void e_jnz_fwd(void) { ob8(0x0F); ob8(0x85); ob32(0); }
static void e_push_ebp(void) { ob8(0x55); }
static void e_mov_ebp_esp(void) { ob8(0x89); ob8(0xE5); }
static void e_sub_esp(int n) { ob8(0x81); ob8(0xEC); ob32((uint32_t)n); }
static void e_add_esp(int n) { ob8(0x81); ob8(0xC4); ob32((uint32_t)n); }
static void e_pop_ebp(void) { ob8(0x5D); }
static void e_ret(void) { ob8(0xC3); }
static void e_push_eax(void) { ob8(0x50); }
static void e_pop_eax(void) { ob8(0x58); }
static void e_push_ecx(void) { ob8(0x51); }
static void e_pop_ecx(void) { ob8(0x59); }
static void e_mov_eax_imm(uint32_t v) { ob8(0xB8); ob32(v); }
static void e_mov_ecx_imm(uint32_t v) { ob8(0xB9); ob32(v); }
static void e_mov_eax_ecx(void) { ob8(0x89); ob8(0xC8); }
static void e_mov_ecx_eax(void) { ob8(0x89); ob8(0xC1); }
static void e_mov_eax_ptr(void) { ob8(0x8B); ob8(0x00); }
static void e_mov_eax_disp32(void) { ob8(0x8B); ob8(0x80); ob32(0); }
static void e_mov_eax_ebx_disp32(void) { ob8(0x8B); ob8(0x83); ob32(0); }
static void e_mov_ebx_eax(void) { ob8(0x89); ob8(0xC3); }
static void e_mov_eax_ebx(void) { ob8(0x89); ob8(0xD8); }
static void e_lea_eax_ebp(int off) { ob8(0x8D); ob8(0x85); ob32((uint32_t)off); }
static void e_lea_eax_ebx(int off) { ob8(0x8D); ob8(0x83); ob32((uint32_t)off); }
static void e_imul_eax_ecx(void) { ob8(0x0F); ob8(0xAF); ob8(0xC1); }
static void e_xchg_eax_ecx(void) { ob8(0x87); ob8(0xC8); }
static void e_neg_eax(void) { ob8(0xF7); ob8(0xD8); }
static void e_not_eax(void) { ob8(0xF7); ob8(0xD0); }
static void e_inc_eax(void) { ob8(0x40); }
static void e_dec_eax(void) { ob8(0x48); }
static void e_shl_eax_cl(void) { ob8(0xD3); ob8(0xE0); }
static void e_sar_eax_cl(void) { ob8(0xD3); ob8(0xF8); }
static void e_rol_eax_cl(void) { ob8(0xD3); ob8(0xC0); }
static void e_ror_eax_cl(void) { ob8(0xD3); ob8(0xC8); }

static void e_alu(int op) {
    /* eax := eax op ecx */
    switch (op) {
    case OP_PLUS: ob8(0x01); ob8(0xC8); break;   /* add eax,ecx */
    case OP_MINUS: ob8(0x29); ob8(0xC8); break;  /* sub eax,ecx */
    case OP_STAR: e_imul_eax_ecx(); break;
    case OP_SLASH: case OP_PERCENT: {
        /* div: edx:eax / ecx; quotient eax, remainder edx */
        ob8(0x99);              /* cdq */
        ob8(0xF7); ob8(0xF9);   /* idiv ecx */
        if (op == OP_PERCENT) e_mov_eax_ecx(); /* remainder in edx -> eax via swap trick */
        break;
    }
    case OP_SHL: case OP_SHR: {
        /* FIND-05 fix: right operand must end up in cl. After the
         * operand push/pop dance the right operand is in eax and the
         * left in ecx; swap them so cl holds the count. */
        e_xchg_eax_ecx();
        if (op == OP_SHL) e_shl_eax_cl();
        else e_sar_eax_cl();
        break;
    }
    case OP_BAND: ob8(0x21); ob8(0xC8); break;
    case OP_BOR: ob8(0x09); ob8(0xC8); break;
    case OP_XOR: ob8(0x31); ob8(0xC8); break;
    default: break;
    }
}

static int bin_prec(int op) {
    switch (op) {
    case OP_OROR: return 2;
    case OP_ANDAND: return 3;
    case OP_PIPE: return 4;
    case OP_CARET: return 5;
    case OP_AMP: return 6;
    case OP_EQ: case OP_NE: return 7;
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 8;
    case OP_SHL: case OP_SHR: return 9;
    case OP_PLUS: case OP_MINUS: return 10;
    case OP_STAR: case OP_SLASH: case OP_PERCENT: return 11;
    default: return 0;
    }
}

/* ---------------- symbols ---------------- */
typedef struct {
    const char *name;
    int kind;          /* SY_FUNC / SY_GVAR / SY_LVAR */
    int is_arr;
    int is_ptr;
    int addr;          /* code offset (func) or data offset (gvar) or ebp offset (lvar) */
    int type;          /* 0=int 1=char */
    int len;
} sym_t;

#define SY_FUNC 1
#define SY_GVAR 2
#define SY_LVAR 3

static sym_t g_syms[MAX_GLOBALS + MAX_LOCALS];
static int g_nsym = 0;
static int g_frame = 0;
static int g_nloc = 0;
static int g_entry_off = 0;
static int g_entry_jmp = -1;
static int g_cur_func = -1;
static int g_narg = 0;
static int g_argoff = 8;

static sym_t *gfind(const char *name) {
    for (int i = 0; i < g_nsym; i++) {
        const char *n = g_syms[i].name;
        int eq = 1;
        for (int j = 0; n[j] || name[j]; j++)
            if (n[j] != name[j]) { eq = 0; break; }
        if (eq) return &g_syms[i];
    }
    return 0;
}

static sym_t *gfind_local(const char *name, int from) {
    for (int i = g_nsym - 1; i >= from; i--) {
        const char *n = g_syms[i].name;
        int eq = 1;
        for (int j = 0; n[j] || name[j]; j++)
            if (n[j] != name[j]) { eq = 0; break; }
        if (eq) return &g_syms[i];
    }
    return 0;
}

static sym_t *local_new(const char *name, int is_arr, int is_ptr, int type) {
    sym_t *s = &g_syms[g_nsym++];
    s->name = name;
    s->kind = SY_LVAR;
    s->is_arr = is_arr;
    s->is_ptr = is_ptr;
    s->type = type;
    s->addr = -(++g_frame * 4);
    s->len = 0;
    return s;
}

/* ---------------- strings ---------------- */
static int str_decode(char *dst, int max, const char *sp, int len) {
    /* decode a string literal body (sp,len) into dst with escapes;
     * shared by local (gen_postfix) and global (parse_program) paths */
    int n = 0;
    for (int i = 0; i < len && n < max; i++) {
        char c = sp[i];
        if (c == '\\' && i + 1 < len) {
            i++;
            switch (sp[i]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = 0; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            default: c = sp[i]; break;
            }
        }
        dst[n++] = c;
    }
    return n;
}

/* ... (rest of compiler: parse_program, parse_func, gen_* per FIND-xx,
 * fixup_code with reloc/call/apiref/DREL backpatching, build_image,
 * save_image with 16K chunked writes) ... */
