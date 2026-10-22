/*
 * ocean_tools.c - Ocean's Tools: a full-screen "better command line" for CatOS.
 * Pure userland CE app. Freestanding i386, no libc, CE01 image.
 * Entry: ce_main at image offset 0. API table via __ce_api slot.
 */
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef signed char    s8;
typedef signed int     s32;

/* ---- CatOS System API v2.2 (33 calls, order matters) ---- */
typedef void  (*fn_write)(const char *s);
typedef void  (*fn_putc)(char c);
typedef void  (*fn_cls)(void);
typedef int   (*fn_readline)(char *buf, int max);
typedef void  (*fn_time_str)(char *out);
typedef int   (*fn_fs_ready)(void);
typedef int   (*fn_ls)(const char *path, void *cb, void *ud);
typedef int   (*fn_touch)(const char *path);
typedef int   (*fn_mkdir)(const char *path);
typedef int   (*fn_cat)(const char *path, char *buf, int max);
typedef int   (*fn_save)(const char *path, const char *data, u32 size);
typedef void *(*fn_alloc)(u32 size);
typedef void  (*fn_free)(void *p);
typedef void  (*fn_exit)(int code);
typedef int   (*fn_net_init)(void);
typedef int   (*fn_net_up)(void);
typedef int   (*fn_http_get)(const char *url, char *buf, int max);
typedef int   (*fn_net_dns)(const char *host, char *ip_out);
typedef int   (*fn_net_udp_send)(const char *ip, int port, const char *data, int len);
typedef int   (*fn_net_udp_recv)(char *src_ip, int *src_port, char *buf, int max);
typedef int   (*fn_net_ping)(const char *ip);
typedef int   (*fn_net_ip)(char *out);
typedef void  (*fn_print_int)(int v);
typedef void  (*fn_print_hex)(u32 v);
typedef void  (*fn_set_fg)(int color);
typedef void  (*fn_delay)(u32 ms);
typedef int   (*fn_atoi)(const char *s);
typedef int   (*fn_strlen)(const char *s);
typedef int   (*fn_strcmp)(const char *a, const char *b);
typedef void  (*fn_strcpy)(char *d, const char *s);
typedef int   (*fn_getch)(void);
typedef int   (*fn_file_exists)(const char *path);
typedef int   (*fn_fsize)(const char *path);

typedef struct ce_api {
    fn_write      write;       /* 0  */
    fn_putc       putc;        /* 1  */
    fn_cls        cls;         /* 2  */
    fn_readline   readline;    /* 3  */
    fn_time_str   time_str;    /* 4  */
    fn_fs_ready   fs_ready;    /* 5  */
    fn_ls         ls;          /* 6  */
    fn_touch      touch;       /* 7  */
    fn_mkdir      mkdir;       /* 8  */
    fn_cat        cat;         /* 9  */
    fn_save       save;        /* 10 */
    fn_alloc      alloc;       /* 11 */
    fn_free       free;        /* 12 */
    fn_exit       exit;        /* 13 */
    /* REAL ABI (v0.2 kernel, verified by slot probing + built-in app
       disassembly): helpers come FIRST, then networking.  The embedded
       API.TXT doc lists them in the reverse order - the doc is wrong. */
    fn_print_int  print_int;   /* 14 */
    fn_print_hex  print_hex;   /* 15 */
    fn_set_fg     set_fg;      /* 16 */
    fn_delay      delay;       /* 17 */
    fn_atoi       atoi;        /* 18 */
    fn_strlen     strlen;      /* 19 */
    fn_strcmp     strcmp;      /* 20 */
    fn_strcpy     strcpy;      /* 21 */
    fn_getch      getch;       /* 22 */
    fn_file_exists file_exists;/* 23 */
    fn_fsize      fsize;       /* 24 */
    fn_net_init   net_init;    /* 25 */
    fn_net_up     net_up;      /* 26 */
    fn_http_get   http_get;    /* 27 */
    fn_net_dns    net_dns;     /* 28 */
    fn_net_udp_send net_udp_send; /* 29 */
    fn_net_udp_recv net_udp_recv; /* 30 */
    fn_net_ping   net_ping;    /* 31 */
    fn_net_ip     net_ip;      /* 32 */
} ce_api_t;

/* kernel writes the API table address into this slot before entry */
static ce_api_t *__ce_api __attribute__((section(".ce_api"))) = (void *)0;

#define api  __ce_api

/* ---- tiny libc ---- */
static u32 my_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (u32)(p - s);
}
static void my_strcpy(char *d, const char *s) {
    while ((*d++ = *s++)) ;
}
static void my_strcat(char *d, const char *s) {
    d += my_strlen(d);
    my_strcpy(d, s);
}
static char *my_strchr(const char *s, int c) {
    do { if (*s == (char)c) return (char *)s; } while (*s++);
    return (char *)0;
}
static void my_memcpy(void *d, const void *s, u32 n) {
    u8 *dp = (u8 *)d; const u8 *sp = (const u8 *)s;
    while (n--) *dp++ = *sp++;
}
static void my_memset(void *d, int c, u32 n) {
    u8 *dp = (u8 *)d;
    while (n--) *dp++ = (u8)c;
}
/* number -> decimal string into buf; returns length (no NUL if full) */
static int u32_to_dec(u32 v, char *buf) {
    char tmp[12]; int i = 0, j = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return j;
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int my_atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static char my_toupper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}
static char my_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}
static int my_memcmp(const void *a, const void *b, u32 n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}
/* VGA 16-color names used by the integrated tools */
#define COL_BLACK 0
#define COL_BLUE  1
#define COL_GREEN 2
#define COL_CYAN  3
#define COL_RED   4
#define COL_MAGENTA 5
#define COL_BROWN 6
#define COL_LGRAY 7
#define COL_DGRAY 8
#define COL_LBLUE 9
#define COL_LGREEN 10
#define COL_LCYAN 11
#define COL_LRED  12
#define COL_LMAGENTA 13
#define COL_YELLOW 14
#define COL_WHITE 15
static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int my_strncmp(const char *a, const char *b, u32 n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}
static char *my_strrchr(const char *s, int c) {
    const char *r = 0;
    do { if (*s == (char)c) r = s; } while (*s++);
    return (char *)r;
}
static int my_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = my_toupper(*a), cb = my_toupper(*b);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++; b++;
    }
    if (!*a && !*b) return 0;
    return *a ? 1 : -1;
}
static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    if (*a < *b) return -1;
    if (*a > *b) return 1;
    return 0;
}
/* trim leading/trailing whitespace in place; returns pointer to trimmed start */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    {
        char *e = s + my_strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        *e = 0;
    }
    return s;
}
/* values: strip leading whitespace and trailing CR only (keep padding spaces) */
static char *trim_val(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    {
        char *e = s + my_strlen(s);
        while (e > s && e[-1] == '\r') e--;
        *e = 0;
    }
    return s;
}

/* ---- output helpers ---- */
static void out(const char *s) { api->write(s); }
static void outln(const char *s) { api->write(s); api->write("\n"); }
static void out_int(int v) {
    char b[16];
    if (v < 0) { api->putc('-'); u32_to_dec((u32)(-(s32)v), b); }
    else u32_to_dec((u32)v, b);
    out(b);
}
static void out_u32(u32 v) {
    char b[16]; u32_to_dec(v, b); out(b);
}
static void out_hex(u32 v) { api->print_hex(v); }
static void set_color(int c) { api->set_fg(c); }

/* split an argument string into argv tokens in place (spaces -> NUL).
   returns argc (tokens); max is the argv capacity. */
static int split_args(const char *args, char (*argv)[40], int max) {
    int n = 0;
    const char *p = args;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        {
            u32 k = 0;
            while (*p && *p != ' ' && k < 39) argv[n][k++] = *p++;
            argv[n][k] = 0;
            n++;
        }
    }
    return n;
}

/* ---- CSC: Ocean's Command Script of CatOS ----
   Line-based scripting language interpreted by Ocean's Tools.
   Grammar (one statement per line, '#' comments):
     :label            - label definition (jump target)
     set NAME = VALUE  - assign a variable (VALUE may use $VAR and + - * /)
     echo [0-15] TEXT  - styled output (same as the echo command)
     goto label        - unconditional jump
     if A op B goto label - op: == != < > <= >= ; numeric compare if both are numbers
     run CMD ...       - run any Ocean's Tools command (with $VAR expansion)
     sleep N           - wait N ticks (kernel delay API)
     anything else     - executed directly as a command line
   $NAME anywhere expands to the variable value.  Variables are per-script. */
#define CSC_MAXLINES 128
#define CSC_LINEMAX 96
#define CSC_MAXVARS 32
#define CSC_MAXLABELS 32
#define CSC_STEPLIMIT 200000
#define CSC_MAXARRS 4
#define CSC_ARRITEMS 64
#define CSC_MAXARGS 9
#define CSC_FORMAX 4
struct csc_var { char name[10]; char val[65]; };
static struct csc_var g_csc_vars[CSC_MAXVARS];
static int g_csc_nvars = 0;
/* CSC 2.0: positional args, arrays, files */
static char g_csc_argv[CSC_MAXARGS + 1][40];
static char g_csc_buf[8192];            /* script file buffer (file-scope, kernel stack is 8KiB) */
static char g_csc_lines[CSC_MAXLINES][CSC_LINEMAX + 1];
static char g_csc_lbl_name[CSC_MAXLABELS][16];
static int  g_csc_lbl_line[CSC_MAXLABELS];
static int  g_csc_active = 0;           /* guards nested script execution */
  /* $0=script path, $1..$9 */
static int g_csc_argc = 0;
static struct csc_var g_csc_ars[CSC_MAXARRS][CSC_ARRITEMS];
static int g_csc_arn[CSC_MAXARRS];
static char g_csc_fa[8192], g_csc_fb[8192];
static u32 g_fa_off[CSC_MAXLINES], g_fb_off[CSC_MAXLINES];
static int g_fa_n = 0, g_fb_n = 0;
/* for-loop stack */
static char g_for_var[CSC_FORMAX][10];
static int g_for_cur[CSC_FORMAX], g_for_end[CSC_FORMAX], g_for_start[CSC_FORMAX];
static int g_for_depth = 0;

static void csc_clear(void) {
    g_csc_nvars = 0;
    g_csc_argc = 0;
    g_fa_n = 0; g_fb_n = 0;
    g_for_depth = 0;
    { int i; for (i = 0; i < CSC_MAXARRS; i++) g_csc_arn[i] = 0; }
}

/* expand $VAR references into out (must not overlap in).  Returns length. */
static int csc_expand_into(const char *in, char *out, int outsz);

static const char *csc_get(const char *name) {
    int i;
    for (i = 0; i < g_csc_nvars; i++)
        if (my_strcmp(g_csc_vars[i].name, name) == 0) return g_csc_vars[i].val;
    /* positional args $0..$9 (numeric names) */
    if (name[0] >= '0' && name[0] <= '9' && name[1] == 0) {
        int a = name[0] - '0';
        if (a <= g_csc_argc) return g_csc_argv[a];
        return "";
    }
    return "";
}

static int is_numstr(const char *s) {
    int i;
    if (!s[0]) return 0;
    for (i = 0; s[i]; i++) if (!is_digit(s[i]) && !(i == 0 && s[i] == '-')) return 0;
    return 1;
}

static const char *csc_arr_get(const char *nm, int idx) {
    int i;
    if (idx < 0) return "";
    for (i = 0; i < CSC_MAXARRS; i++)
        if (g_csc_arn[i] > 0 && my_strcmp(g_csc_ars[i][0].name, nm) == 0) {
            if (idx < g_csc_arn[i]) return g_csc_ars[i][idx].val;
            return "";
        }
    return "";
}

static void csc_arr_set(const char *nm, int idx, const char *val) {
    int i, slot = -1;
    for (i = 0; i < CSC_MAXARRS; i++)
        if (g_csc_arn[i] > 0 && my_strcmp(g_csc_ars[i][0].name, nm) == 0) { slot = i; break; }
    if (slot < 0) {
        for (i = 0; i < CSC_MAXARRS; i++)
            if (g_csc_arn[i] == 0) { slot = i; break; }
        if (slot < 0) { outln("csc: too many arrays"); return; }
        my_strcpy(g_csc_ars[slot][0].name, nm);
        g_csc_arn[slot] = 0;
    }
    if (idx < 0) idx = 0;
    if (idx >= CSC_ARRITEMS) { outln("csc: array index out of range"); return; }
    while (g_csc_arn[slot] <= idx && g_csc_arn[slot] < CSC_ARRITEMS) {
        g_csc_ars[slot][g_csc_arn[slot]].val[0] = 0;
        g_csc_arn[slot]++;
    }
    my_strcpy(g_csc_ars[slot][idx].val, val);
}

static void csc_set(const char *name, const char *raw) {
    char ex[80], a[40], b[40];
    int i, oi = 0, ai, bi;
    char op = 0;
    int anum, bnum;
    if (!name[0]) return;
    csc_expand_into(raw, ex, (int)sizeof(ex));
    /* arithmetic: "<num> op <num>" with op in + - * / % */
    ai = 0; bi = 0;
    while (ex[oi] == ' ') oi++;
    while (ex[oi] && ex[oi] != ' ' && ai < 39) a[ai++] = ex[oi++];
    a[ai] = 0;
    while (ex[oi] == ' ') oi++;
    if (ex[oi] == '+' || ex[oi] == '-' || ex[oi] == '*' || ex[oi] == '/' || ex[oi] == '%') {
        op = ex[oi++];
        while (ex[oi] == ' ') oi++;
        while (ex[oi] && ex[oi] != ' ' && bi < 39) b[bi++] = ex[oi++];
        b[bi] = 0;
        anum = is_numstr(a); bnum = is_numstr(b);
        if (anum && bnum) {
            int va = my_atoi(a), vb = my_atoi(b), vr = 0;
            if (vb == 0 && (op == '/' || op == '%')) { outln("csc: division by zero"); return; }
            if (op == '+') vr = va + vb;
            else if (op == '-') vr = va - vb;
            else if (op == '*') vr = va * vb;
            else if (op == '/') vr = va / vb;
            else if (op == '%') vr = va % vb;
            u32_to_dec((u32)vr, ex);
            /* store */
            for (i = 0; i < g_csc_nvars; i++)
                if (my_strcmp(g_csc_vars[i].name, name) == 0) {
                    my_strcpy(g_csc_vars[i].val, ex);
                    return;
                }
            if (g_csc_nvars < CSC_MAXVARS) {
                my_strcpy(g_csc_vars[g_csc_nvars].name, name);
                my_strcpy(g_csc_vars[g_csc_nvars].val, ex);
                g_csc_nvars++;
            }
            return;
        }
    }
    /* plain string value */
    for (i = 0; i < g_csc_nvars; i++)
        if (my_strcmp(g_csc_vars[i].name, name) == 0) {
            my_strcpy(g_csc_vars[i].val, ex);
            return;
        }
    if (g_csc_nvars < CSC_MAXVARS) {
        my_strcpy(g_csc_vars[g_csc_nvars].name, name);
        my_strcpy(g_csc_vars[g_csc_nvars].val, ex);
        g_csc_nvars++;
    }
}

/* expand $VAR / $N (args) / $ARR[IDX] references into out. Returns length. */
static int csc_expand_into(const char *in, char *out, int outsz) {
    int oi = 0;
    while (*in && oi < outsz - 1) {
        char c = *in++;
        if (c == '$' && ((is_alpha(*in) || *in == '_') || (*in >= '0' && *in <= '9'))) {
            char nm[16];
            int ni = 0;
            while (*in && ni < 14 && (is_alnum(*in) || *in == '_')) nm[ni++] = *in++;
            nm[ni] = 0;
            if (*in == '[') {           /* array ref: $a[idx] */
                char ix[16];
                int ii = 0, idx;
                in++;
                while (*in && *in != ']' && ii < 14) ix[ii++] = *in++;
                ix[ii] = 0;
                if (*in == ']') in++;
                {
                    char iex[20];
                    csc_expand_into(ix, iex, (int)sizeof(iex));
                    idx = my_atoi(iex);
                }
                {
                    const char *v = csc_arr_get(nm, idx);
                    while (*v && oi < outsz - 1) out[oi++] = *v++;
                }
            } else {
                const char *v = csc_get(nm);
                while (*v && oi < outsz - 1) out[oi++] = *v++;
            }
        } else {
            out[oi++] = c;
        }
    }
    out[oi] = 0;
    return oi;
}

/* ---- CSC 2.0 helpers: files, strings, hex ---- */
static int resolve_existing(const char *arg, char *p, int psz);

static const char *my_strstr(const char *hay, const char *needle) {
    int hl = my_strlen(hay), nl = my_strlen(needle);
    int i;
    if (nl == 0) return hay;
    if (nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++)
        if (my_strncmp(hay + i, needle, nl) == 0) return hay + i;
    return 0;
}

/* external commands (/bin/*.csc) - variable table kept near csc helpers */
#define EXT_MAX 16
static char g_ext_cmds[EXT_MAX][16];
static int g_ext_n = 0;

static int csc_save_shards(const char *fname, const char *data, u32 len)
{
    u32 off = 0;
    int idx = 0;
    if (len <= 512) {
        if (api->save(fname, data, len) < 0) return 0;
        return 1;
    }
    for (;;) {
        u32 chunk = len - off;
        if (chunk > 512) chunk = 512;
        char path[160], num[3];
        my_strcpy(path, fname);
        my_strcat(path, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(path, num);
        if (api->save(path, data + off, chunk) < 0) return 0;
        off += chunk;
        idx++;
        if (off >= len) break;
    }
    return 1;
}

/* build line-start offsets for a buffer; returns line count */
static int csc_index_lines(const char *buf, int n, u32 *offs, int maxl)
{
    int k = 0, i;
    if (n == 0) return 0;
    offs[0] = 0;
    k = 1;
    for (i = 0; i < n; i++)
        if (buf[i] == '\n' && k < maxl) offs[k++] = (u32)(i + 1);
    return k;
}

static void csc_fetch_row(const char *buf, int n, const u32 *offs, int k,
                          int r, char *out, int outsz)
{
    u32 s, e;
    int i, L;
    if (r < 0 || r >= k) { out[0] = 0; return; }
    s = offs[r];
    e = (r + 1 < k) ? offs[r + 1] : (u32)n;
    if (e > s && buf[e - 1] == '\n') e--;
    L = (int)(e - s);
    if (L > outsz - 1) L = outsz - 1;
    for (i = 0; i < L; i++) out[i] = buf[s + i];
    out[L] = 0;
}

/* numeric string for hex output */
static void csc_hex(u32 v, char *out, int outsz)
{
    char tmp[16];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && i < 14) {
        int d = v & 0xF;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    tmp[i] = 0;
    /* reverse */
    {
        int j;
        for (j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
        out[i] = 0;
        if (i > outsz - 1) out[outsz - 1] = 0;
    }
}

static void csc_to_upper(const char *in, char *out, int outsz)
{
    int i = 0;
    while (*in && i < outsz - 1) {
        char c = *in++;
        out[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[i] = 0;
}

static void csc_to_lower(const char *in, char *out, int outsz)
{
    int i = 0;
    while (*in && i < outsz - 1) {
        char c = *in++;
        out[i++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[i] = 0;
}

/* numeric if both operands are plain numbers, else string compare */
static int csc_test_cond(const char *a, const char *op, const char *b) {
    int anum = 1, bnum = 1, i;
    for (i = 0; a[i]; i++) if (!is_digit(a[i]) && !(i == 0 && a[i] == '-')) { anum = 0; break; }
    for (i = 0; b[i]; i++) if (!is_digit(b[i]) && !(i == 0 && b[i] == '-')) { bnum = 0; break; }
    if (anum && bnum) {
        int va = my_atoi(a), vb = my_atoi(b);
        if (my_strcmp(op, "==") == 0) return va == vb;
        if (my_strcmp(op, "!=") == 0) return va != vb;
        if (my_strcmp(op, "<=") == 0) return va <= vb;
        if (my_strcmp(op, ">=") == 0) return va >= vb;
        if (my_strcmp(op, "<") == 0) return va < vb;
        if (my_strcmp(op, ">") == 0) return va > vb;
        return 0;
    }
    {
        int r = my_strcmp(a, b);
        if (my_strcmp(op, "==") == 0) return r == 0;
        if (my_strcmp(op, "!=") == 0) return r != 0;
        if (my_strcmp(op, "<") == 0) return r < 0;
        if (my_strcmp(op, ">") == 0) return r > 0;
        if (my_strcmp(op, "<=") == 0) return r <= 0;
        if (my_strcmp(op, ">=") == 0) return r >= 0;
    }
    return 0;
}

static void run_script(const char *path);
static void run_script_args(const char *path, char (*av)[40], int ac);
static void cmd_echo(const char *arg);
static void run_line(char *line);

static void run_script(const char *path) { run_script_args(path, 0, 0); }

static void run_script_args(const char *path, char (*av)[40], int ac) {
    int nlbl = 0, nlines = 0, pc, steps = 0;
    char *p;
    if (g_csc_active) { outln("csc: nested scripts not supported"); return; }
    g_csc_active = 1;
    int n = api->cat(path, g_csc_buf, (int)sizeof(g_csc_buf) - 1);
    if (n < 0) {
        out("csc: cannot read ");
        outln(path);
        return;
    }
    g_csc_buf[n] = 0;
    csc_clear();
    g_csc_argc = ac;
    my_strcpy(g_csc_argv[0], path);
    if (av) {
        int i;
        for (i = 0; i < ac && i < CSC_MAXARGS; i++) my_strcpy(g_csc_argv[i + 1], av[i]);
    } else {
        g_csc_argc = 0;
    }
    out("[csc] ");
    outln(path);
    p = g_csc_buf;
    while (*p && nlines < CSC_MAXLINES) {
        char *eol = my_strchr(p, '\n');
        u32 L = eol ? (u32)(eol - p) : my_strlen(p);
        if (L > CSC_LINEMAX) L = CSC_LINEMAX;
        my_memcpy(g_csc_lines[nlines], p, L);
        g_csc_lines[nlines][L] = 0;
        nlines++;
        p = eol ? eol + 1 : p + L;
    }
    /* pre-scan labels */
    for (pc = 0; pc < nlines; pc++) {
        char *t = trim(g_csc_lines[pc]);
        if (t[0] == ':') {
            char *nm = t + 1;
            u32 k = 0;
            while (nm[k] == ' ' || nm[k] == '\t') k++;
            if (nlbl < CSC_MAXLABELS) {
                u32 c = 0;
                while (nm[k] && nm[k] != ' ' && c < 14) g_csc_lbl_name[nlbl][c++] = nm[k++];
                g_csc_lbl_name[nlbl][c] = 0;
                g_csc_lbl_line[nlbl] = pc;
                nlbl++;
            }
        }
    }
    pc = 0;
    while (pc < nlines) {
        char *t = trim(g_csc_lines[pc]);
        char arg[120];
        int done = 0;
        if (steps++ > CSC_STEPLIMIT) {
            outln("csc: step limit exceeded (infinite loop?)");
            break;
        }
        if (t[0] == 0 || t[0] == '#' || t[0] == ':') { pc++; continue; }
        if (my_strncmp(t, "set ", 4) == 0) {
            char *eq = my_strchr(t + 4, '=');
            char nm[16];
            u32 c = 0;
            const char *np = t + 4;
            while (*np == ' ') np++;
            while (c < 15) {
                char ch = np[c];
                if (!ch || ch == ' ' || ch == '=' || ch == '[') break;
                nm[c] = ch;
                c++;
            }
            nm[c] = 0;
            if (eq && nm[0]) {
                /* array assignment: set a[idx] = VAL */
                if (np[c] == '[') {
                    char ix[16];
                    u32 ii = 0, k2 = c + 1;
                    int idx;
                    while (np[k2] && np[k2] != ']' && ii < 14) ix[ii++] = np[k2++];
                    ix[ii] = 0;
                    {
                        char iex[20];
                        csc_expand_into(ix, iex, (int)sizeof(iex));
                        idx = my_atoi(iex);
                    }
                    {
                        char vx[65];
                        const char *v = eq + 1;
                        while (*v == ' ') v++;
                        csc_expand_into(v, vx, (int)sizeof(vx));
                        csc_arr_set(nm, idx, vx);
                    }
                } else {
                    const char *v = eq + 1;
                    while (*v == ' ') v++;
                    csc_set(nm, v);
                }
            } else {
                outln("csc: usage: set NAME = VALUE");
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "echo ", 5) == 0) {
            char ex[400];
            csc_expand_into(t + 5, ex, (int)sizeof(ex));
            cmd_echo(ex);
            pc++;
            continue;
        }
        if (my_strncmp(t, "goto ", 5) == 0) {
            char nm[16];
            u32 c = 0, k = 5;
            int i;
            while (t[k] == ' ' || t[k] == '\t') k++;
            while (t[k] && t[k] != ' ' && c < 14) nm[c++] = t[k++];
            nm[c] = 0;
            done = 0;
            for (i = 0; i < nlbl; i++)
                if (my_strcmp(g_csc_lbl_name[i], nm) == 0) { pc = g_csc_lbl_line[i]; done = 1; break; }
            if (!done) { out("csc: no such label: "); outln(nm); break; }
            continue;
        }
        if (my_strncmp(t, "if ", 3) == 0) {
            /* if A op B goto label */
            char a[40], b[40], op[4];
            int ai = 0, bi = 0, oi = 0, k = 3, i;
            int found = 0;
            char *gl = 0;
            while (t[k] == ' ') k++;
            while (t[k] && t[k] != ' ' && ai < 39) a[ai++] = t[k++];
            a[ai] = 0;
            while (t[k] == ' ') k++;
            if (t[k] == '=' && t[k + 1] == '=') { op[oi++] = '='; op[oi++] = '='; k += 2; }
            else if (t[k] == '!' && t[k + 1] == '=') { op[oi++] = '!'; op[oi++] = '='; k += 2; }
            else if (t[k] == '<' && t[k + 1] == '=') { op[oi++] = '<'; op[oi++] = '='; k += 2; }
            else if (t[k] == '>' && t[k + 1] == '=') { op[oi++] = '>'; op[oi++] = '='; k += 2; }
            else if (t[k] == '<' || t[k] == '>') { op[oi++] = t[k++]; }
            op[oi] = 0;
            while (t[k] == ' ') k++;
            while (t[k] && t[k] != ' ' && bi < 39) b[bi++] = t[k++];
            b[bi] = 0;
            while (t[k] == ' ') k++;
            if (my_strncmp(t + k, "goto ", 5) == 0) gl = t + k + 5;
            while (gl && *gl == ' ') gl++;
            if (!op[0] || !gl) {
                outln("csc: usage: if A op B goto label");
                pc++;
                continue;
            }
            {
                char ea[80], eb[80];
                csc_expand_into(a, ea, (int)sizeof(ea));
                csc_expand_into(b, eb, (int)sizeof(eb));
                found = csc_test_cond(ea, op, eb);
            }
            if (found) {
                char nm[16];
                u32 c = 0;
                while (gl[c] && gl[c] != ' ' && c < 14) {
                    nm[c] = gl[c];
                    c++;
                }
                nm[c] = 0;
                done = 0;
                for (i = 0; i < nlbl; i++)
                    if (my_strcmp(g_csc_lbl_name[i], nm) == 0) { pc = g_csc_lbl_line[i]; done = 1; break; }
                if (!done) { out("csc: no such label: "); outln(nm); break; }
                continue;
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "sleep ", 6) == 0) {
            int ms = my_atoi(t + 6);
            if (ms > 0 && ms < 60000) api->delay((u32)ms);
            pc++;
            continue;
        }
        /* ---- CSC 2.0: for / next ---- */
        if (my_strncmp(t, "for ", 4) == 0) {
            /* for VAR = START to END */
            char vn[10], sa[16], ea[16];
            u32 ci = 4, si = 0;
            int start, end;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && t[ci] != '=' && si < 9) vn[si++] = t[ci++];
            vn[si] = 0;
            while (t[ci] == ' ' || t[ci] == '=') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) sa[si++] = t[ci++];
            sa[si] = 0;
            while (t[ci] && t[ci] != 't') ci++;
            if (my_strncmp(t + ci, "to", 2) == 0) ci += 2;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) ea[si++] = t[ci++];
            ea[si] = 0;
            {
                char sx[20], ex2[20];
                csc_expand_into(sa, sx, (int)sizeof(sx));
                csc_expand_into(ea, ex2, (int)sizeof(ex2));
                start = my_atoi(sx);
                end = my_atoi(ex2);
            }
            if (g_for_depth >= CSC_FORMAX) { outln("csc: for nesting too deep"); break; }
            my_strcpy(g_for_var[g_for_depth], vn);
            g_for_start[g_for_depth] = pc + 1;
            g_for_cur[g_for_depth] = start;
            g_for_end[g_for_depth] = end;
            g_for_depth++;
            /* initialize loop var */
            {
                char vx[20];
                u32_to_dec((u32)start, vx);
                csc_set(vn, vx);
            }
            pc++;
            continue;
        }
        if (my_strcmp(t, "next") == 0) {
            if (g_for_depth > 0) {
                int d = g_for_depth - 1;
                char vx[20];
                g_for_cur[d]++;
                u32_to_dec((u32)g_for_cur[d], vx);
                csc_set(g_for_var[d], vx);
                if (g_for_cur[d] <= g_for_end[d]) {
                    pc = g_for_start[d];
                    continue;
                }
                g_for_depth--;
            } else {
                outln("csc: next without for");
            }
            pc++;
            continue;
        }
        /* ---- CSC 2.0: string functions (var-name style: S/VAR are var
               names, NOT expanded; numeric args are expanded) ---- */
        if (my_strncmp(t, "len ", 4) == 0) {
            char S[12], VAR[12], rv[20];
            u32 ci = 4, si = 0, vi = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) S[si++] = t[ci++];
            S[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (S[0] && VAR[0]) {
                u32_to_dec((u32)my_strlen(csc_get(S)), rv);
                csc_set(VAR, rv);
            } else outln("csc: usage: len S VAR");
            pc++;
            continue;
        }
        if (my_strncmp(t, "upper ", 6) == 0 || my_strncmp(t, "lower ", 6) == 0) {
            char S[12], VAR[12], buf2[80];
            u32 ci = (t[1] == 'p') ? 6 : 6, si = 0, vi = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) S[si++] = t[ci++];
            S[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (S[0] && VAR[0]) {
                if (t[1] == 'p') csc_to_upper(csc_get(S), buf2, (int)sizeof(buf2));
                else csc_to_lower(csc_get(S), buf2, (int)sizeof(buf2));
                csc_set(VAR, buf2);
            } else outln("csc: usage: upper S VAR | lower S VAR");
            pc++;
            continue;
        }
        if (my_strncmp(t, "substr ", 7) == 0) {
            char S[12], I[16], N[16], VAR[12], buf2[80];
            u32 ci = 7, si = 0, vi = 0;
            int pos, len2, i3, L;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) S[si++] = t[ci++];
            S[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) I[si++] = t[ci++];
            I[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) N[si++] = t[ci++];
            N[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (S[0] && VAR[0]) {
                char iex[20], nex[20];
                csc_expand_into(I, iex, (int)sizeof(iex));
                csc_expand_into(N, nex, (int)sizeof(nex));
                pos = my_atoi(iex);
                len2 = my_atoi(nex);
                if (pos < 0) pos = 0;
                L = my_strlen(csc_get(S));
                for (i3 = 0; i3 < len2 && pos + i3 < L && i3 < (int)sizeof(buf2) - 1; i3++)
                    buf2[i3] = csc_get(S)[pos + i3];
                buf2[i3] = 0;
                csc_set(VAR, buf2);
            } else outln("csc: usage: substr S I N VAR");
            pc++;
            continue;
        }
        if (my_strncmp(t, "find ", 5) == 0) {
            char S[12], P[40], VAR[12];
            u32 ci = 5, si = 0, vi = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) S[si++] = t[ci++];
            S[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 38) P[si++] = t[ci++];
            P[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (S[0] && VAR[0]) {
                char pex[40], rv[20];
                char *f;
                csc_expand_into(P, pex, (int)sizeof(pex));
                f = my_strstr(csc_get(S), pex);
                if (f) u32_to_dec((u32)(f - csc_get(S)), rv);
                else my_strcpy(rv, "-1");
                csc_set(VAR, rv);
            } else outln("csc: usage: find S P VAR");
            pc++;
            continue;
        }
        /* tok S N VAR - N-th whitespace token of var S (0-based) */
        if (my_strncmp(t, "tok ", 4) == 0) {
            char S[12], N[16], VAR[12];
            u32 ci = 4, si = 0, vi = 0;
            int idx, tok_i = 0;
            const char *v;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) S[si++] = t[ci++];
            S[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) N[si++] = t[ci++];
            N[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (S[0] && VAR[0]) {
                char nex[20];
                csc_expand_into(N, nex, (int)sizeof(nex));
                idx = my_atoi(nex);
                v = csc_get(S);
                while (*v && tok_i < idx) {
                    while (*v == ' ') v++;
                    while (*v && *v != ' ') v++;
                    tok_i++;
                }
                while (*v == ' ') v++;
                if (tok_i != idx || !*v) csc_set(VAR, "");
                else {
                    char buf2[40];
                    u32 o = 0;
                    while (*v && *v != ' ' && o < 38) buf2[o++] = *v++;
                    buf2[o] = 0;
                    csc_set(VAR, buf2);
                }
            } else outln("csc: usage: tok S N VAR");
            pc++;
            continue;
        }
        /* same VA VB label - jump if var VA == var VB (whole values) */
        if (my_strncmp(t, "same ", 5) == 0) {
            char VA[12], VB[12], LB[16];
            u32 ci = 5, si = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 10) VA[si++] = t[ci++];
            VA[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 10) VB[si++] = t[ci++];
            VB[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) LB[si++] = t[ci++];
            LB[si] = 0;
            if (VA[0] && VB[0] && LB[0]) {
                if (my_strcmp(csc_get(VA), csc_get(VB)) == 0) {
                    int i2;
                    done = 0;
                    for (i2 = 0; i2 < nlbl; i2++)
                        if (my_strcmp(g_csc_lbl_name[i2], LB) == 0) { pc = g_csc_lbl_line[i2]; done = 1; break; }
                    if (!done) { out("csc: no such label: "); outln(LB); break; }
                    continue;
                }
            } else outln("csc: usage: same VA VB label");
            pc++;
            continue;
        }
        /* ---- CSC 2.0: files ---- */
        if (my_strncmp(t, "load ", 5) == 0 || my_strncmp(t, "loadb ", 6) == 0) {
            char p[128];
            int isb = (t[1] == 'o' && t[2] == 'a' && t[3] == 'd' && t[4] == 'b');
            const char *ps = t + (isb ? 6 : 5);
            while (*ps == ' ') ps++;
            csc_expand_into(ps, p, (int)sizeof(p));
            {
                char rp[128];
                char *fbuf = isb ? g_csc_fb : g_csc_fa;
                u32 *offs = isb ? g_fb_off : g_fa_off;
                int *cnt = isb ? &g_fb_n : &g_fa_n;
                int rn;
                resolve_existing(p, rp, (int)sizeof(rp));
                rn = api->cat(rp, fbuf, 8191);
                if (rn < 0) { out("csc: cannot read "); outln(rp); }
                else {
                    *cnt = csc_index_lines(fbuf, rn, offs, CSC_MAXLINES);
                    if (rn >= 8191) outln("csc: note: file >8K truncated");
                }
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "rows ", 5) == 0) {
            char T[4], VAR[12];
            u32 ci = 5, si = 0, vi = 0;
            int isb = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 3) T[si++] = t[ci++];
            T[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (T[0] == 'B' || T[0] == 'b') isb = 1;
            {
                char rv[20];
                u32_to_dec((u32)(isb ? g_fb_n : g_fa_n), rv);
                csc_set(VAR[0] ? VAR : T, rv);
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "row ", 4) == 0) {
            char T[4], N[16], VAR[12];
            u32 ci = 4, si = 0, vi = 0;
            int isb = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && si < 3) T[si++] = t[ci++];
            T[si] = 0;
            while (t[ci] == ' ') ci++;
            si = 0;
            while (t[ci] && t[ci] != ' ' && si < 14) N[si++] = t[ci++];
            N[si] = 0;
            while (t[ci] == ' ') ci++;
            while (t[ci] && t[ci] != ' ' && vi < 10) VAR[vi++] = t[ci++];
            VAR[vi] = 0;
            if (T[0] == 'B' || T[0] == 'b') isb = 1;
            {
                char nex[20], buf2[80];
                int r;
                const char *buf;
                const u32 *offs;
                int k, n;
                csc_expand_into(N, nex, (int)sizeof(nex));
                r = my_atoi(nex);
                buf = isb ? g_csc_fb : g_csc_fa;
                offs = isb ? g_fb_off : g_fa_off;
                k = isb ? g_fb_n : g_fa_n;
                n = isb ? (int)sizeof(g_csc_fb) : (int)sizeof(g_csc_fa);
                csc_fetch_row(buf, n, offs, k, r, buf2, (int)sizeof(buf2));
                csc_set(VAR[0] ? VAR : T, buf2);
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "write ", 6) == 0) {
            char p[128];
            const char *ps = t + 6;
            while (*ps == ' ') ps++;
            /* split path and text */
            {
                char *sp = (char *)ps;
                char rp[128];
                u32 i2 = 0;
                while (*sp && *sp != ' ' && i2 < 127) p[i2++] = *sp++;
                p[i2] = 0;
                while (*sp == ' ') sp++;
                csc_expand_into(p, rp, (int)sizeof(rp));
                {
                    char tx[512];
                    csc_expand_into(sp, tx, (int)sizeof(tx));
                    if (!csc_save_shards(rp, tx, my_strlen(tx)))
                        outln("csc: write failed");
                }
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "hex ", 4) == 0) {
            char a[80], rv[20];
            int k = 4;
            while (t[k] == ' ') k++;
            csc_expand_into(t + k, a, (int)sizeof(a));
            {
                char *sp = a, *last = a;
                int v;
                while (*sp) { if (*sp == ' ') last = sp + 1; sp++; }
                v = my_atoi(a);
                csc_hex((u32)v, rv, (int)sizeof(rv));
                csc_set(last, rv);
            }
            pc++;
            continue;
        }
        if (my_strncmp(t, "run ", 4) == 0) {
            csc_expand_into(t + 4, arg, (int)sizeof(arg));
            run_line(arg);
            pc++;
            continue;
        }
        /* default: run as a command line */
        csc_expand_into(t, arg, (int)sizeof(arg));
        run_line(arg);
        pc++;
    }
    outln("[csc] end");
    g_csc_active = 0;
}

/* ---- banner ---- */
static const char *BANNER[] = {
    "",
    " ###    ###   #####   ###   #   #  #   ###    #####   ###    ###   #       ### ",
    "#   #  #   #  #      #   #  ##  #  #  #         #    #   #  #   #  #      #    ",
    "#   #  #      ####   #####  # # #  #   ###      #    #   #  #   #  #       ### ",
    "#   #  #   #  #      #   #  #  ##         #     #    #   #  #   #  #          #",
    " ###    ###   #####  #   #  #   #      ###      #     ###    ###   #####   ### ",
    "==============================================================================",
    "",  /* row 7: tagline comes from the config file */
    "",
};

/* ---- configuration ----
   Loaded from OCNTOOLS.CFG in the same directory as ocntools.ce (/bin)
   on every startup.  Missing file or key = built-in default. */
struct cfg {
    char title[40];
    char tagline[96];
    char welcome[96];
    char info_boot[32];
    char info_fs[32];
    char info_net[32];
    char hint[80];
    char prompt[24];
    int color_title;
    int color_tagline;
    int color_prompt;
    char startup[8][80];
    int n_startup;
};
static struct cfg CFG;

static void cfg_defaults(void) {
    my_strcpy(CFG.title, "OCEAN'S TOOLS");
    my_strcpy(CFG.tagline, "a better command line for CatOS - userland CE app (v1.6)");
    my_strcpy(CFG.welcome, "Welcome to Ocean's Tools. A better CatOS command line.");
    my_strcpy(CFG.info_boot, "Boot time  : ");
    my_strcpy(CFG.info_fs, "Filesystem : ");
    my_strcpy(CFG.info_net, "Network    : ");
    my_strcpy(CFG.hint, "Type 'help', or just a file name to open it.");
    my_strcpy(CFG.prompt, "ocean");
    CFG.color_title = 9;    /* light blue */
    CFG.color_tagline = 14; /* yellow */
    CFG.color_prompt = 11;  /* light cyan */
    CFG.n_startup = 0;
}

static void cfg_set(const char *key, const char *val) {
    if (my_strcasecmp(key, "title") == 0) {
        if (val[0]) my_strcpy(CFG.title, val);
    } else if (my_strcasecmp(key, "tagline") == 0) {
        if (val[0]) my_strcpy(CFG.tagline, val);
    } else if (my_strcasecmp(key, "welcome") == 0) {
        if (val[0]) my_strcpy(CFG.welcome, val);
    } else if (my_strcasecmp(key, "info_boot") == 0) {
        if (val[0]) my_strcpy(CFG.info_boot, val);
    } else if (my_strcasecmp(key, "info_fs") == 0) {
        if (val[0]) my_strcpy(CFG.info_fs, val);
    } else if (my_strcasecmp(key, "info_net") == 0) {
        if (val[0]) my_strcpy(CFG.info_net, val);
    } else if (my_strcasecmp(key, "hint") == 0) {
        if (val[0]) my_strcpy(CFG.hint, val);
    } else if (my_strcasecmp(key, "prompt") == 0) {
        if (val[0]) my_strcpy(CFG.prompt, val);
    } else if (my_strcasecmp(key, "color_title") == 0) {
        int c = api->atoi(val);
        if (c >= 0 && c <= 15) CFG.color_title = c;
    } else if (my_strcasecmp(key, "color_tagline") == 0) {
        int c = api->atoi(val);
        if (c >= 0 && c <= 15) CFG.color_tagline = c;
    } else if (my_strcasecmp(key, "color_prompt") == 0) {
        int c = api->atoi(val);
        if (c >= 0 && c <= 15) CFG.color_prompt = c;
    } else if (my_strcasecmp(key, "startup_cmd") == 0) {
        if (val[0] && CFG.n_startup < 8) {
            u32 l = my_strlen(val);
            if (l > 79) l = 79;
            my_memcpy(CFG.startup[CFG.n_startup], val, l);
            CFG.startup[CFG.n_startup][l] = 0;
            CFG.n_startup++;
        }
    }
}

/* read and parse /bin/OCNTOOLS.CFG; silently falls back to defaults */
static void cfg_load(void) {
    char buf[8192];
    char *p;
    int n;
    cfg_defaults();
    n = api->cat("/bin/OCNTOOLS.CFG", buf, (int)sizeof(buf) - 1);
    if (n < 0) return;
    buf[n] = 0;
    p = buf;
    while (*p) {
        char *eol = my_strchr(p, '\n');
        char line[256];
        u32 L = eol ? (u32)(eol - p) : my_strlen(p);
        char *eq, *key, *val;
        if (L > 255) L = 255;
        my_memcpy(line, p, L);
        line[L] = 0;
        p = eol ? eol + 1 : p + L;
        /* skip blank / comment lines */
        {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == 0 || *t == '#') continue;
        }
        eq = my_strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(line);
        val = trim_val(eq + 1);
        if (!key[0]) continue;
        cfg_set(key, val);
    }
}

/* ---- state ---- */
static char cwd[128] = "/";

/* ---- path resolve: /a/../b -> canonical absolute path ---- */
static void resolve_path(const char *arg, char *out, int outsz) {
    char tmp[256];
    u32 i = 0;
    if (arg[0] == '/') {
        u32 k = 0;
        while (arg[k] && k < 250) { tmp[i++] = arg[k++]; }
    } else {
        u32 k = 0;
        while (cwd[k] && k < 120 && i < 250) { tmp[i++] = cwd[k]; k++; }
        if (k > 0 && cwd[k - 1] != '/') tmp[i++] = '/';
        u32 m = 0;
        while (arg[m] && m < 120 && i < 250) { tmp[i++] = arg[m]; m++; }
    }
    tmp[i] = 0;
    /* collapse . and .. */
    {
        char segs[16][32];
        int nseg = 0, j;
        char *p = tmp;
        while (*p) {
            char *e = my_strchr(p, '/');
            u32 len = e ? (u32)(e - p) : my_strlen(p);
            if (len == 0) { p = e ? e + 1 : p + my_strlen(p); continue; }
            if (len == 1 && p[0] == '.') { p = e ? e + 1 : p + 1; continue; }
            if (len == 2 && p[0] == '.' && p[1] == '.') {
                if (nseg > 0) nseg--;
                p = e ? e + 1 : p + 2;
                continue;
            }
            if (nseg < 16) {
                u32 c = 0;
                while (c < len && c < 31) { segs[nseg][c] = p[c]; c++; }
                segs[nseg][c] = 0;
                nseg++;
            }
            p = e ? e + 1 : p + len;
        }
        out[0] = '/';
        out[1] = 0;   /* fix: out must be NUL-terminated before my_strcat scans it */
        for (j = 0; j < nseg && j < 8; j++) {
            u32 m = 0;
            u32 need = my_strlen(segs[j]) + 1;
            if (outsz - (s32)my_strlen(out) < (s32)need + 1) break;
            if (j > 0) my_strcat(out, "/");
            else my_strcat(out, "");
            {
                char *dst = out + my_strlen(out);
                while (segs[j][m]) { dst[m] = segs[j][m]; m++; }
                dst[m] = 0;
            }
        }
        if (nseg == 0) my_strcpy(out, "/");
    }
}

/* ---- ls callback ---- */
struct ls_ctx { int count; int is_dir_first; };
static void ls_cb(int is_dir, const char *name, u32 size, void *ud) {
    struct ls_ctx *c = (struct ls_ctx *)ud;
    if (is_dir) {
        set_color(11);
        out("[DIR ] ");
        set_color(15);
    } else {
        set_color(10);
        out("[FILE] ");
        set_color(15);
    }
    out(name);
    if (!is_dir) {
        out(" (");
        out_u32(size);
        out(" bytes)");
    }
    out("\n");
    c->count++;
}

/* ---- commands ---- */
static void cmd_help(const char *arg);
static void cmd_banner(const char *arg);
static void cmd_about(const char *arg);
static void cmd_dir(const char *arg);
static void cmd_cd(const char *arg);
static void cmd_cat(const char *arg);
static void cmd_edit(const char *arg);
static void cmd_curl(const char *arg);
static void cmd_ping(const char *arg);
static void cmd_dns(const char *arg);
static void cmd_udp(const char *arg);
static void cmd_netinfo(const char *arg);
static void cmd_mem(const char *arg);
static void cmd_files(const char *arg);
static void cmd_unknown(const char *cmd);
static void cmd_kernelonly(const char *what);
static void cmd_alias(const char *arg);
static void cmd_cecheck(const char *arg);
static int resolve_existing(const char *arg, char *p, int psz);

static void cmd_banner(const char *arg) { (void)arg;
    int row;
    set_color(7);
    if (my_strcasecmp(CFG.title, "OCEAN'S TOOLS") == 0) {
        /* built-in pixel art: light-blue letters, white separator.
           a2: art is 79 cols; center on 80 cols by a 1-col left offset. */
        for (row = 0; row < 7; row++) {
            if (row >= 1 && row <= 5) set_color(CFG.color_title);
            else if (row == 6) set_color(7);
            out(" ");
            outln(BANNER[row]);
        }
    } else {
        /* custom title: plain text mode */
        set_color(CFG.color_title);
        outln(CFG.title);
        set_color(7);
        outln("");
        outln("==============================================================================");
    }
    set_color(CFG.color_tagline);
    outln(CFG.tagline);
    set_color(15);
}

static void cmd_about(const char *arg) { (void)arg;
    cmd_banner("");
    outln("");
    set_color(11);
    outln("Ocean's Tools v1.6 - userland full-screen command line for CatOS");
    set_color(15);
    outln("  - every CatOS builtin that the CE API exposes, plus a few extras");
    outln("  - all commands run in user mode: no kernel changes required");
    outln("  - echo supports all 16 VGA colors: 'echo 9 hello'");
    outln("  - CSC scripts (.csc): variables, if/goto, styled echo, run any cmd");
    outln("  - shortcut: typing a file name opens it (.csc runs, text shows)");
    outln("  - config: /bin/OCNTOOLS.CFG (title, tagline, welcome, info, startup)");
    outln("  - type 'help' for the full command list, 'exit' to return to CatOS");
}

static void cmd_help(const char *arg) { (void)arg;
    int i;
    outln("");
    set_color(14); outln("Ocean's Tools commands (CE API v2.2):");
    set_color(11);
    outln("  help/?        this list                  clear       clear screen");
    outln("  echo [0-15] TEXT  styled print           about       show banner+info");
    outln("  uname         OS name                    date/time   show RTC time");
    outln("  color NUM     set color 0-15             colors      preview palette");
    outln("  dir [PATH]    list directory             cd [PATH]   change dir");
    outln("  mkdir PATH    create dir                 touch PATH  create empty file");
    outln("  cat PATH      print a file               edit PATH   line editor");
    outln("  files PATH    exists/size info           mem         heap probe");
    outln("  ping IP|NAME  ICMP echo (3 tries)        dns NAME    resolve host");
    outln("  curl URL      HTTP GET (IP[:port])       udp IP PORT TEXT  send+wait");
    outln("  netinfo       NIC status / own IP");
    set_color(11);
    outln("  cecheck PATH  inspect a .CE image (CE01 header health)");
    set_color(12);
    outln("  csc PATH      run a .CSC script          run PATH    run .CSC (or kernel .CE)");
    outln("  sft PAT F [-i] [-c] [-r]  sift search    vcs init|add|commit|log|status|diff|checkout");
    outln("  cedbg FILE [hdr|hex|dis|str|mem|sym|self]  inspect .CE images");
    outln("  edt PATH  editpp editor   fm  file manager");
    outln("  reboot / hang / format / netdiag   (kernel-only: no CE API)");
    set_color(14);
    outln("  exit / quit / bye                        return to the CatOS shell");
    outln("  bare file name: .CSC runs it, other files are shown (cat)");
    set_color(13);
    if (g_ext_n > 0) {
        out("  external commands: ");
        for (i = 0; i < g_ext_n; i++) {
            if (i > 0) out(" ");
            out(g_ext_cmds[i]);
        }
        outln("");
    }
    set_color(15);
    outln("Program lookup: builtin -> ./NAME.ce -> /bin/NAME.ce (same as CatOS)");
}

static void cmd_clear(const char *arg) { (void)arg; api->cls(); }

static void cmd_echo(const char *arg) {
    /* echo [0-15] TEXT
       - optional first token 0..15 sets the foreground color (all 16
         VGA colors supported; reset to white after)
       - $NAME expands a CSC variable (empty outside scripts)
       - literal "\n" (backslash-n) prints a newline */
    char obuf[600];
    int oi = 0, col = -1;
    const char *p = arg;
    if (is_digit(p[0])) {
        int c = my_atoi(p);
        if (c >= 0 && c <= 15) {
            col = c;
            while (is_digit(*p)) p++;
            while (*p == ' ') p++;
        }
    }
    if (col >= 0) set_color(col);
    while (*p && oi < (int)sizeof(obuf) - 2) {
        char c = *p++;
        if (c == '$' && (is_alpha(*p) || *p == '_')) {
            char nm[16];
            int ni = 0;
            while (*p && ni < 14 && (is_alnum(*p) || *p == '_')) nm[ni++] = *p++;
            nm[ni] = 0;
            {
                const char *v = csc_get(nm);
                while (*v && oi < (int)sizeof(obuf) - 2) obuf[oi++] = *v++;
            }
        } else if (c == '\\' && *p == 'n') {
            p++;
            if (oi < (int)sizeof(obuf) - 1) obuf[oi++] = '\n';
        } else {
            obuf[oi++] = c;
        }
    }
    obuf[oi] = 0;
    out(obuf);
    api->putc('\n');
    set_color(15);
}

static void cmd_uname(const char *arg) { (void)arg;
    set_color(11); out("CatOS ");
    set_color(15); out("i386 ");
    set_color(10); out("0.2");
    set_color(15); outln(" (run by Ocean's Tools v1.6, CE userland)");
}

static void cmd_date(const char *arg) {
    char t[32];
    (void)arg;
    api->time_str(t);
    out("Current RTC: ");
    outln(t);
}

static void cmd_color(const char *arg) {
    int c = api->atoi(arg);
    if (c < 0 || c > 15) { outln("usage: color 0..15"); return; }
    api->set_fg(c);
    out("color set to ");
    out_int(c);
    outln("");
}

static void cmd_colors(const char *arg) { (void)arg;
    int c;
    for (c = 0; c < 16; c++) {
        api->set_fg(c);
        out("  ");
        out_int(c);
        if (c < 10) out(" ");
        out(" ");
    }
    api->set_fg(15);
    outln("");
    outln("0=black 1=blue 2=green 3=cyan 4=red 5=magenta 6=brown 7=gray");
    outln("8=darkgray 9=lblue 10=lgreen 11=lcyan 12=lred 13=lmagenta 14=yellow 15=white");
}

static void cmd_dir(const char *arg) {
    struct ls_ctx ctx;
    char p[128];
    if (!api->fs_ready()) { outln("catos: no FAT32 volume. Run 'format' first."); return; }
    resolve_path(arg[0] ? arg : ".", p, sizeof(p));
    ctx.count = 0;
    if (api->ls(p, ls_cb, &ctx) != 0) {
        out("catos: dir: no such directory: ");
        outln(p);
        return;
    }
    out("--- ");
    out_u32((u32)ctx.count);
    outln(" entries ---");
}

static void cmd_cd(const char *arg) {
    char p[128];
    struct ls_ctx ctx;
    if (!arg[0]) { outln(cwd); return; }
    resolve_path(arg, p, sizeof(p));
    ctx.count = 0;
    if (api->ls(p, ls_cb, &ctx) != 0) {
        out("cd: not a directory: ");
        outln(p);
        return;
    }
    my_strcpy(cwd, p);
    out("now at ");
    outln(cwd);
}

static void cmd_mkdir(const char *arg) {
    char p[128];
    int rc;
    if (!arg[0]) { outln("usage: mkdir PATH"); return; }
    resolve_path(arg, p, sizeof(p));
    rc = api->mkdir(p);
    if (rc == 0) { out("created "); outln(p); }
    else if (rc == -5) { out("already exists: "); outln(p); }
    else { out("mkdir failed ("); out_int(rc); out("): "); outln(p); }
}

static void cmd_touch(const char *arg) {
    char p[128];
    int rc;
    if (!arg[0]) { outln("usage: touch PATH"); return; }
    resolve_path(arg, p, sizeof(p));
    rc = api->touch(p);
    if (rc == 0) { out("touched "); outln(p); }
    else { out("touch failed ("); out_int(rc); out("): "); outln(p); }
}

static void cmd_cat(const char *arg) {
    char p[128], buf[5120];
    int n;
    if (!arg[0]) { outln("usage: cat PATH"); return; }
    resolve_path(arg, p, sizeof(p));
    n = api->cat(p, buf, (int)sizeof(buf));
    if (n < 0) { out("cat: cannot read "); outln(p); return; }
    out("---- ");
    out(p);
    out(" (");
    out_int(n);
    outln(" bytes) ----");
    buf[n] = 0;
    out(buf);
    if (n > 0 && buf[n - 1] != '\n') out("\n");
}

/* ---- line editor: minimal nano-style, lines-based ---- */
#define ED_MAXLINES 64
#define ED_MAXCH 79
static char ed_lines[ED_MAXLINES][ED_MAXCH + 2];
static int ed_nlines = 0;
static char ed_path[128];

static int ed_load(const char *path) {
    char big[8192];
    int n = api->cat(path, big, (int)sizeof(big));
    int li = 0, ci = 0, i;
    if (n < 0) { ed_nlines = 0; return 0; }
    big[n] = 0;
    for (i = 0; i <= n && li < ED_MAXLINES; i++) {
        char c = big[i];
        if (c == '\n' || c == 0) {
            ed_lines[li][ci] = 0;
            li++;
            ci = 0;
            if (c == 0) break;
        } else if (ci < ED_MAXCH) {
            ed_lines[li][ci++] = c;
        }
    }
    ed_nlines = li;
    return n;
}

static void ed_show(void) {
    int i;
    char num[8];
    for (i = 0; i < ed_nlines; i++) {
        u32_to_dec((u32)i + 1, num);
        set_color(11);
        out("[");
        out(num);
        out("] ");
        set_color(15);
        out(ed_lines[i]);
        out("\n");
    }
    out("--- ");
    out_u32((u32)ed_nlines);
    outln(" lines ---");
}

static int ed_save(void) {
    char big[8192];
    int len = 0, i;
    for (i = 0; i < ed_nlines; i++) {
        u32 l = my_strlen(ed_lines[i]);
        u32 c;
        for (c = 0; c < l && len < (int)sizeof(big) - 2; c++)
            big[len++] = ed_lines[i][c];
        if (len < (int)sizeof(big) - 1) big[len++] = '\n';
    }
    if (len > 512) {
        outln("SAVE FAILED: exceeds 512-byte file limit (kernel save() API)");
        return -1;
    }
    if (api->save(ed_path, big, (u32)len) == 0) {
        out("saved ");
        out_int(len);
        outln(" bytes");
        return 0;
    }
    outln("save failed");
    return -1;
}

static void cmd_edit(const char *arg) {
    char line[128];
    int run = 1;
    if (!arg[0]) { outln("usage: edit PATH"); return; }
    resolve_path(arg, ed_path, sizeof(ed_path));
    ed_load(ed_path);
    out("CatOS nano-ocean: ");
    outln(ed_path);
    ed_show();
    outln("commands: L<num> <text>=replace  A <text>=append  D<num>=delete  O=save  X=save&quit  Q=quit");
    while (run) {
        out("ed> ");
        if (api->readline(line, (int)sizeof(line)) <= 0) continue;
        if (line[0] == 'X' && line[1] == 0) { ed_save(); run = 0; }
        else if (line[0] == 'O' && line[1] == 0) { ed_save(); }
        else if (line[0] == 'Q' && line[1] == 0) { run = 0; }
        else if (line[0] == 'A' && line[1] == ' ') {
            const char *t = line + 2;
            if (ed_nlines < ED_MAXLINES) {
                u32 l = my_strlen(t);
                if (l > ED_MAXCH) l = ED_MAXCH;
                my_memcpy(ed_lines[ed_nlines], t, l);
                ed_lines[ed_nlines][l] = 0;
                ed_nlines++;
                outln("appended");
            } else outln("Too many lines (max 64)");
        }
        else if (line[0] == 'D' && line[1] >= '0' && line[1] <= '9') {
            int num = my_atoi(line + 1);
            if (num >= 1 && num <= ed_nlines) {
                int i;
                for (i = num - 1; i < ed_nlines - 1; i++)
                    my_strcpy(ed_lines[i], ed_lines[i + 1]);
                ed_nlines--;
                outln("deleted");
            } else outln("no such line");
        }
        else if (line[0] == 'L' && line[1] >= '0' && line[1] <= '9') {
            char *sp = my_strchr(line, ' ');
            int num = my_atoi(line + 1);
            if (sp && num >= 1 && num <= ed_nlines) {
                const char *t = sp + 1;
                u32 l = my_strlen(t);
                if (l > ED_MAXCH) l = ED_MAXCH;
                my_memcpy(ed_lines[num - 1], t, l);
                ed_lines[num - 1][l] = 0;
                outln("replaced");
            } else outln("usage: L<num> <new text>");
        }
        else if (line[0] == 'S' && line[1] == 0) { ed_show(); }
        else outln("? (L# text / A text / D# / O / X / Q / S)");
    }
}

/* ---- network ---- */
static void cmd_curl(const char *arg) {
    static char buf[4096];
    int n;
    if (!arg[0]) { outln("usage: curl http://IP[:port]/path"); return; }
    if (!api->net_up()) { outln("curl: network not ready (add -device pcnet)"); return; }
    out("fetching ");
    outln(arg);
    n = api->http_get(arg, buf, (int)sizeof(buf));
    if (n < 0) {
        out("curl: error ");
        out_int(n);
        outln(" (1=NIC 2=host 3=connect 4=http 5=resp 6=send)");
        return;
    }
    outln("--- response ---");
    buf[n] = 0;
    out(buf);
    if (n > 0 && buf[n - 1] != '\n') out("\n");
    out("--- done: ");
    out_int(n);
    outln(" bytes ---");
}

static int looks_ip(const char *s) {
    int dots = 0;
    while (*s) {
        if (*s == '.') dots++;
        else if (!is_digit(*s)) return 0;
        s++;
    }
    return dots == 3;
}

static void cmd_ping(const char *arg) {
    char ip[32];
    int rc;
    if (!arg[0]) { outln("usage: ping IP_or_host"); return; }
    if (!api->net_up()) { outln("ping: network not ready"); return; }
    if (looks_ip(arg)) my_strcpy(ip, arg);
    else {
        out("resolving ");
        outln(arg);
        if (api->net_dns(arg, ip) != 0) { outln("dns: could not resolve host"); return; }
        out(ip); outln("");
    }
    out("ping ");
    outln(ip);
    rc = api->net_ping(ip);
    if (rc == 0) { set_color(10); outln("reply"); }
    else { set_color(12); outln("no reply"); }
    set_color(15);
}

static void cmd_dns(const char *arg) {
    char ip[32];
    if (!arg[0]) { outln("usage: dns hostname"); return; }
    if (!api->net_up()) { outln("dns: network not ready"); return; }
    if (api->net_dns(arg, ip) != 0) {
        out("dns: could not resolve '");
        out(arg);
        outln("'");
        return;
    }
    out(arg);
    out(" -> ");
    outln(ip);
}

static void cmd_udp(const char *arg) {
    /* udp IP PORT TEXT */
    char ip[32], text[80];
    int port, len, rc;
    char reply[512], src[24];
    int srcport = 0;
    if (!arg[0]) { outln("usage: udp IP PORT TEXT"); return; }
    if (!api->net_up()) { outln("udp: network not ready"); return; }
    {
        const char *p = arg;
        u32 i = 0;
        while (*p && *p != ' ' && i < 31) ip[i++] = *p++;
        ip[i] = 0;
        while (*p == ' ') p++;
        port = my_atoi(p);
        while (is_digit(*p)) p++;
        while (*p == ' ') p++;
        i = 0;
        while (*p && i < 79) text[i++] = *p++;
        text[i] = 0;
    }
    if (!ip[0] || port <= 0 || port > 65535 || !text[0]) {
        outln("usage: udp IP PORT TEXT");
        return;
    }
    out("udp -> ");
    out(ip);
    out(":");
    out_int(port);
    out(" '");
    out(text);
    outln("'");
    len = my_strlen(text);
    rc = api->net_udp_send(ip, port, text, len);
    if (rc != 0) { outln("udp: send failed"); return; }
    outln("sent, waiting for reply...");
    rc = api->net_udp_recv(src, &srcport, reply, (int)sizeof(reply));
    if (rc < 0) { outln("udp: no reply"); return; }
    out("reply from ");
    out(src);
    out(":");
    out_int(srcport);
    outln("");
    reply[rc] = 0;
    out(reply);
    outln("");
}

static void cmd_netinfo(const char *arg) {
    char ip[32];
    (void)arg;
    if (!api->net_up()) {
        outln("network: no network card found (add -device pcnet)");
        return;
    }
    out("NIC      : AMD PCnet-PCI up\n");
    api->net_ip(ip);
    out("my ip    : ");
    outln(ip);
    out("DNS test : example.com -> ");
    if (api->net_dns("example.com", ip) == 0) outln(ip);
    else outln("(failed)");
}

static void cmd_mem(const char *arg) { (void)arg;
    /* no CE API for the multiboot memory map; do a small heap probe */
    void *p = api->alloc(4096);
    if (p) {
        out("heap probe: alloc 4096 ok @ ");
        out_hex((u32)p);
        outln("");
        api->free(p);
    } else {
        outln("heap probe: alloc failed (pool exhausted?)");
    }
    outln("(full RAM size is kernel-only; try the CatOS 'mem' builtin)");
}

static void cmd_files(const char *arg) {
    char p[128];
    if (!arg[0]) { outln("usage: files PATH"); return; }
    resolve_path(arg, p, sizeof(p));
    if (api->file_exists(p)) {
        out(p);
        out(" exists, size = ");
        out_int(api->fsize(p));
        outln(" bytes");
    } else {
        out("not found: ");
        outln(p);
    }
}

/* cecheck PATH - inspect a CE01 image (the "run .ce from ot" feasibility
   tool): reads the file, validates the CE01 header and prints the load
   layout.  ot cannot execute .ce in userland (fixed link base 0x02000000,
   no fixed-address alloc, api->exit() relaunches the same image), so this
   is the userland-safe equivalent: verify before the kernel runs it. */
static void cmd_cecheck(const char *arg) {
    char p[128];
    u32 sz;
    char *buf;
    if (!arg[0]) { outln("usage: cecheck PATH.CE"); return; }
    if (!resolve_existing(arg, p, (int)sizeof(p))) {
        out("cecheck: not found: ");
        outln(arg);
        return;
    }
    sz = (u32)api->fsize(p);
    if (sz < 20) { outln("cecheck: file too small to be a CE image"); return; }
    buf = (char *)api->alloc(sz + 1);
    if (!buf) { outln("cecheck: alloc failed"); return; }
    if (api->cat(p, buf, (int)(sz + 1)) < 0) {
        api->free(buf);
        outln("cecheck: read failed");
        return;
    }
    out(p);
    out(": ");
    if (buf[0] == 'C' && buf[1] == 'E' && buf[2] == '0' && buf[3] == '1') {
        u32 entry, bss, api_off, img_sz;
        my_memcpy(&entry, buf + 4, 4);
        my_memcpy(&bss, buf + 8, 4);
        my_memcpy(&api_off, buf + 12, 4);
        my_memcpy(&img_sz, buf + 16, 4);
        set_color(10);
        outln("valid CE01 image");
        set_color(15);
        out("  entry     = ");
        out_hex(entry);
        outln("");
        out("  bss       = ");
        out_u32(bss);
        outln(" bytes");
        out("  api table = +0x");
        { char tmp[32]; int i;
          for (i = 7; i >= 0; i--) tmp[7 - i] = "0123456789abcdef"[(api_off >> (i * 4)) & 15];
          tmp[8] = 0;
          out(tmp); }
        outln(" (link base 0x02000000)");
        out("  image     = ");
        out_u32(img_sz);
        out(" bytes, file ");
        out_u32(sz);
        outln(" bytes");
        if (api_off > img_sz || img_sz > sz) {
            set_color(12);
            outln("  WARNING: header fields out of range - image likely corrupt");
            set_color(15);
        }
        outln("  run it from the CatOS shell: exit, then run <path>");
    } else {
        set_color(12);
        outln("NOT a CE01 image (no CE01 magic)");
        set_color(15);
        outln("cecheck reads .CSC/.TXT fine - this is just not a CE program.");
    }
    api->free(buf);
}

static void cmd_kernelonly(const char *what) {
    out(what);
    outln(" is a kernel builtin: no CE API exists for it in userland.");
    outln("use the CatOS shell prompt for this (press Ctrl+... no, type 'exit' first).");
}

/* resolve a path; if not found relative to cwd, fall back to /bin/NAME */
static int resolve_existing(const char *arg, char *p, int psz) {
    char b[160];
    resolve_path(arg, p, psz);
    if (api->file_exists(p)) return 1;
    if (arg[0] != '/') {
        b[0] = '/'; b[1] = 'b'; b[2] = 'i'; b[3] = 'n'; b[4] = '/';
        my_strcpy(b + 5, arg);
        if (api->file_exists(b)) {
            my_strcpy(p, b);
            return 1;
        }
    }
    return 0;
}

static void cmd_run(const char *arg) {
    char p[128];
    char av[9][40];
    int ac;
    if (!arg[0]) {
        outln("usage: run PATH   (.csc scripts execute; .ce needs the kernel shell)");
        return;
    }
    {
        char line[160];
        my_strcpy(line, arg);
        ac = split_args(line, av, 9);
    }
    resolve_existing(av[0], p, sizeof(p));
    {
        const char *e = my_strrchr(p, '.');
        if (e && my_strcasecmp(e, ".csc") == 0) {
            run_script_args(p, ac > 1 ? &av[1] : 0, ac > 1 ? ac - 1 : 0);
            return;
        }
    }
    cmd_kernelonly("run");
    outln("tip: type 'exit' to return to the CatOS shell, then 'run /bin/NAME.CE'.");
}
static void cmd_csc(const char *arg) {
    char p[160];
    char av[9][40];
    int ac;
    if (!arg[0]) { outln("usage: csc PATH.CSC [ARGS...]"); return; }
    {
        char line[160];
        my_strcpy(line, arg);
        ac = split_args(line, av, 9);
    }
    resolve_existing(av[0], p, sizeof(p));
    run_script_args(p, ac > 1 ? &av[1] : 0, ac > 1 ? ac - 1 : 0);
}
static void cmd_reboot(const char *arg) { (void)arg; cmd_kernelonly("reboot"); }
static void cmd_hang(const char *arg)   { (void)arg; cmd_kernelonly("hang"); }
static void cmd_format(const char *arg) { (void)arg; cmd_kernelonly("format"); }
static void cmd_netdiag(const char *arg){ (void)arg; cmd_kernelonly("netdiag"); }

/* =====================================================================
   Integrated tools (catos-ce-tools v1.0): sft vcs cedbg edt fm -
   built in as ot commands (diff/vk moved OUT to external .csc in v1.6).
   Each keeps its original logic with a tool-specific prefix
   (tg_/tv_/tc_/thex_/tf_); shared libc (out/outln/my_str*) is
   reused from ot above.
   ===================================================================== */

/* ---------- grep (tg_) ---------- */
static int tg_icase = 0, tg_count = 0, tg_recursive = 0;
static const char *tg_pat = 0;
static u32 tg_total_hits = 0, tg_skipped = 0, tg_dir_depth = 0;
static char tg_curdir[64];
static char tg_pattern[64];
/* shared 16K work buffer for the integrated tools (grep/plot/vcs use it;
   they never run concurrently).  Keeping big buffers out of the 8K stack
   and sharing them keeps image+bss well under limits. */
static char tool_buf[16384];

static int tg_ch_eq(int a, int b) {
    if (tg_icase) { a = my_toupper((char)a); b = my_toupper((char)b); }
    return a == b;
}
static int tg_wild(const char *p, const char *s) {
    for (;;) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (tg_wild(p, s)) return 1;
            return tg_wild(p, s);
        }
        if (!*s) return *p == 0;
        if (*p == '?') { p++; s++; continue; }
        if (!tg_ch_eq(*p, *s)) return 0;
        p++; s++;
    }
}
static void tg_scan_buffer(const char *path, const char *buf, u32 len) {
    u32 line_no = 1, i = 0, hits = 0;
    while (i < len) {
        u32 linelen = 0;
        char line[256];
        while (i < len && buf[i] != '\n' && linelen < 250) line[linelen++] = buf[i++];
        if (i < len && buf[i] == '\n') i++;
        else while (i < len && linelen < 250) line[linelen++] = buf[i++];
        line[linelen] = 0;
        {
            int hit;
            if (my_strchr(tg_pat, '*') || my_strchr(tg_pat, '?')) {
                hit = tg_wild(tg_pat, line);
            } else {
                u32 k, m;
                hit = 0;
                for (k = 0; line[k]; k++) {
                    for (m = 0; tg_pat[m]; m++)
                        if (!tg_ch_eq(line[k + m], tg_pat[m])) break;
                    if (tg_pat[m] == 0) { hit = 1; break; }
                    if (!line[k + 1]) break;
                }
            }
            if (hit) {
                hits++;
                tg_total_hits++;
                if (!tg_count) {
                    out(path); out(":"); out_u32(line_no); out(": ");
                    outln(line);
                }
            }
        }
        line_no++;
    }
    if (tg_count && hits) {
        out(path); out(": "); out_u32(hits); outln(" line(s)");
    }
}
struct tg_dentry { char name[13]; u8 is_dir; };
static struct tg_dentry tg_dlist[64];
static u32 tg_dcount = 0;
static int tg_ls_cb(int is_dir, const char *name, u32 size, void *ud) {
    (void)size; (void)ud;
    if (tg_dcount < 64) {
        u32 i;
        for (i = 0; i < tg_dcount; i++)
            if (my_strcmp(tg_dlist[i].name, name) == 0) return 0;
        my_strcpy(tg_dlist[tg_dcount].name, name);
        tg_dlist[tg_dcount].is_dir = (u8)is_dir;
        tg_dcount++;
    }
    return 0;
}
static void tg_scan_file(const char *path) {
    int sz = api->fsize(path);
    int n;
    if (sz < 0) return;
    if (sz > 8192) {
        tg_skipped++;
        if (!tg_count) { out("skip(>8KB): "); outln(path); }
        return;
    }
    n = api->cat(path, tool_buf, 8192);
    if (n < 0) { if (!tg_count) { out("cannot read: "); outln(path); } return; }
    if (n > 8192) n = 8192;
    tg_scan_buffer(path, tool_buf, (u32)n);
}
static void tg_scan_dir(const char *dir) {
    char path[160];
    u32 i;
    if (tg_dir_depth >= 4) return;
    tg_dir_depth++;
    tg_dcount = 0;
    api->ls(dir, (void *)tg_ls_cb, 0);
    for (i = 0; i < tg_dcount; i++) {
        if (tg_dlist[i].is_dir) continue;
        if (tg_dlist[i].name[0] == '.') continue;
        if (tg_curdir[0]) my_strcpy(path, tg_curdir);
        else path[0] = 0;
        my_strcat(path, tg_dlist[i].name);
        tg_scan_file(path);
    }
    for (i = 0; i < tg_dcount; i++) {
        if (!tg_dlist[i].is_dir) continue;
        if (tg_dlist[i].name[0] == '.') continue;
        my_strcpy(path, tg_curdir);
        my_strcat(path, tg_dlist[i].name);
        my_strcat(path, "/");
        my_strcpy(tg_curdir, path);
        tg_scan_dir(path);
        tg_curdir[0] = 0;
    }
    tg_dir_depth--;
}
static void cmd_grep(const char *arg) {
    char av[12][40];
    int ac = split_args(arg, av, 12);
    int i;
    tg_icase = 0; tg_count = 0; tg_recursive = 0;
    tg_pat = 0; tg_total_hits = 0; tg_skipped = 0; tg_dir_depth = 0;
    tg_curdir[0] = 0;
    for (i = 0; i < ac; i++) {
        const char *a = av[i];
        if (a[0] == '-' && a[1] && !a[2]) {
            if (a[1] == 'i') tg_icase = 1;
            else if (a[1] == 'c') tg_count = 1;
            else if (a[1] == 'r') tg_recursive = 1;
            else { out("sft: unknown option: "); outln(a); return; }
        } else if (!tg_pat) {
            if (my_strlen(a) < sizeof(tg_pattern)) {
                my_strcpy(tg_pattern, a);
                tg_pat = tg_pattern;
            } else { outln("sft: pattern too long"); return; }
        } else {
            char p[128];
            if (tg_recursive && api->fsize(a) < 0) tg_scan_dir(a);
            else {
                resolve_existing(a, p, (int)sizeof(p));
                tg_scan_file(p);
            }
        }
    }
    if (!tg_pat) {
        outln("usage: sft PATTERN FILE... [-i icase] [-c count] [-r recurse]");
        return;
    }
    if (tg_skipped) {
        out("sft: skipped "); out_u32(tg_skipped); outln(" file(s) >8KB (cat limit)");
    }
    set_color(11);
    out("sft: "); out_u32(tg_total_hits); outln(" hit line(s)");
    set_color(15);
}

/* ---------- vcs (tv_) lightweight version control ----------
   subcommands: init | add F | commit MSG | log | status | diff F | checkout NN
   (short aliases i/a/c/l/s/d/co).  Metadata lives in .VCS/ under the ot cwd;
   snapshots are sharded (<=512B per save call).  Paths resolved via ot cwd. */
#define TV_DIR ".VCS"
#define TV_INFO TV_DIR "/INFO"
#define TV_INDEX TV_DIR "/INDEX"
#define TV_LOG TV_DIR "/LOG"
#define TV_MAX_FILES 40
#define TV_MAX_SHARD 512
#define tv_wbuf tool_buf
#define tv_sbbuf (tool_buf + 8192)
/* D1 fix: slots must hold a resolve_existing-resolved absolute path (up to
   ~127 chars), not a 16-char basename.  Widened 16->120; BSS budget allows it
   (40*(120-16)=+4160; 61124+4160<65536).  120 chars exceeds every realistic
   path (segments capped at 31 chars; defect trigger "/work/report.txt"=16). */
#define TV_NAME_SZ 120
static char tv_names[TV_MAX_FILES][TV_NAME_SZ];
static int tv_n = 0;

static int tv_save_shards(const char *base, const char *data, u32 len) {
    u32 off = 0;
    int idx = 0;
    for (;;) {
        u32 chunk = len - off;
        char path[96], num[3];
        if (chunk > TV_MAX_SHARD) chunk = TV_MAX_SHARD;
        my_strcpy(path, base);
        my_strcat(path, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(path, num);
        if (api->save(path, data + off, chunk) < 0) {
            out("vcs: save failed: "); outln(path);
            return 0;
        }
        off += chunk;
        idx++;
        if (off >= len) break;
    }
    return 1;
}
static int tv_load_shards(const char *base, char *buf, u32 max) {
    u32 total = 0;
    int idx = 0;
    for (;;) {
        char path[96], num[3];
        int n;
        my_strcpy(path, base);
        my_strcat(path, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(path, num);
        if (!api->file_exists(path)) break;
        n = api->cat(path, buf + total, (int)(max - total));
        if (n < 0) { out("vcs: read failed: "); outln(path); return -1; }
        total += (u32)n;
        if (total >= max) break;
        idx++;
        if (idx > 40) break;
    }
    return (int)total;
}
static void tv_load_index(void) {
    char buf[1024];
    char *p;
    int n;
    tv_n = 0;
    {
        char pth[128];
        resolve_path(TV_INDEX, pth, (int)sizeof(pth));
        if (!api->file_exists(pth)) return;
        n = tv_load_shards(pth, buf, sizeof(buf));
    }
    if (n < 0) return;
    buf[n] = 0;
    p = buf;
    while (*p && tv_n < TV_MAX_FILES) {
        char *nl = my_strchr(p, '\n');
        char *sp;
        char *name;
        u32 l, k;
        if (!nl) nl = p + my_strlen(p);
        sp = my_strchr(p, ' ');
        name = sp ? sp + 1 : p;
        l = (u32)(nl - name);
        if (l > TV_NAME_SZ - 1) l = TV_NAME_SZ - 1;
        for (k = 0; k < l; k++) tv_names[tv_n][k] = name[k];
        tv_names[tv_n][l] = 0;
        tv_n++;
        p = nl;
        if (*p == '\n') p++;
    }
}
static void tv_save_index(void) {
    char buf[1024];
    char pth[128];
    u32 pos = 0;
    int i;
    resolve_path(TV_INDEX, pth, (int)sizeof(pth));
    for (i = 0; i < tv_n; i++) {
        char num[4];
        u32 k;
        u32_to_dec((u32)i, num);
        for (k = 0; num[k]; k++) buf[pos++] = num[k];
        buf[pos++] = ' ';
        for (k = 0; tv_names[i][k]; k++) buf[pos++] = tv_names[i][k];
        buf[pos++] = '\n';
    }
    tv_save_shards(pth, buf, pos);
}
static int tv_get_head(void) {
    char buf[64];
    char pth[128];
    int n;
    char *sp;
    resolve_path(TV_INFO, pth, (int)sizeof(pth));
    if (!api->file_exists(pth)) return -1;
    n = api->cat(pth, buf, sizeof(buf));
    if (n < 0) return -1;
    buf[n] = 0;
    sp = my_strchr(buf, ':');
    return sp ? my_atoi(sp + 1) : -1;
}
static void tv_set_head(int v) {
    char buf[32], num[8], pth[128];
    my_strcpy(buf, "VCS1 rev:");
    u32_to_dec((u32)v, num);
    my_strcat(buf, num);
    my_strcat(buf, "\n");
    resolve_path(TV_INFO, pth, (int)sizeof(pth));
    api->save(pth, buf, my_strlen(buf));
}
static void tv_init(void) {
    char pth[128];
    resolve_path(TV_DIR, pth, (int)sizeof(pth));
    if (api->file_exists(pth)) { outln("vcs: already initialized (.vcs exists)"); return; }
    api->mkdir(pth);
    tv_set_head(0);
    {
        char p1[128], p2[128];
        resolve_path(TV_INDEX, p1, (int)sizeof(p1));
        resolve_path(TV_LOG, p2, (int)sizeof(p2));
        api->save(p1, "", 0);
        api->save(p2, "", 0);
    }
    outln("vcs: initialized .vcs repo");
}
static void tv_add(const char *name) {
    char pth[128];
    int i;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    if (!name[0]) { outln("usage: vcs add <file>"); return; }
    resolve_existing(name, pth, (int)sizeof(pth));
    if (!api->file_exists(pth)) { out("vcs: file not found: "); outln(name); return; }
    tv_load_index();
    for (i = 0; i < tv_n; i++)
        if (my_strcmp(tv_names[i], pth) == 0) { out("vcs: already tracked: "); outln(pth); return; }
    if (tv_n >= TV_MAX_FILES) { outln("vcs: tracked file limit 40"); return; }
    /* D1 fix: bounded copy, guarantee NUL termination, never overflow the slot
       even if pth (char[128]) is longer than TV_NAME_SZ-1. */
    {
        u32 k = 0;
        while (pth[k] && k < TV_NAME_SZ - 1) { tv_names[tv_n][k] = pth[k]; k++; }
        tv_names[tv_n][k] = 0;
    }
    tv_n++;
    tv_save_index();
    out("vcs: tracked: "); outln(name);
}
static void tv_commit(const char *msg) {
    char dir[128], num[3], pth[128];
    int head, nv, i;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    tv_load_index();
    if (tv_n == 0) { outln("vcs: no tracked files (vcs add first)"); return; }
    head = tv_get_head();
    nv = head + 1;
    my_strcpy(dir, TV_DIR "/REV");
    num[0] = (char)('0' + nv / 10);
    num[1] = (char)('0' + nv % 10);
    num[2] = 0;
    my_strcat(dir, num);
    resolve_path(dir, pth, (int)sizeof(pth));
    api->mkdir(pth);
    for (i = 0; i < tv_n; i++) {
        char base[160], fnum[3], fp[128];
        int sz, n;
        my_strcpy(base, pth);
        my_strcat(base, "/F");
        fnum[0] = (char)('0' + i / 10);
        fnum[1] = (char)('0' + i % 10);
        fnum[2] = 0;
        my_strcat(base, fnum);
        resolve_path(tv_names[i], fp, (int)sizeof(fp));
        sz = api->fsize(fp);
        if (sz < 0) { out("vcs: missing, skip: "); outln(tv_names[i]); continue; }
        if (sz > 8192) { out("vcs: >8KB (cat limit), skip: "); outln(tv_names[i]); continue; }
        n = api->cat(fp, tv_wbuf, 8192);
        if (n < 0) { out("vcs: read failed: "); outln(tv_names[i]); continue; }
        tv_save_shards(base, tv_wbuf, (u32)n);
    }
    tv_set_head(nv);
    {
        char logbuf[1024], line[300], t[64], nnum[4], lp[128];
        u32 l, m;
        int n;
        api->time_str(t);
        u32_to_dec((u32)nv, nnum);
        my_strcpy(line, nnum);
        my_strcat(line, "|");
        my_strcat(line, t);
        my_strcat(line, "|");
        my_strcat(line, msg);
        my_strcat(line, "\n");
        resolve_path(TV_LOG, lp, (int)sizeof(lp));
        n = tv_load_shards(lp, logbuf, sizeof(logbuf));
        if (n < 0) n = 0;
        logbuf[n] = 0;
        l = my_strlen(logbuf);
        if (l + my_strlen(line) > sizeof(logbuf) - 1) l = sizeof(logbuf) - 1 - my_strlen(line);
        for (m = 0; line[m] && l < sizeof(logbuf) - 1; m++) logbuf[l++] = line[m];
        tv_save_shards(lp, logbuf, l);
    }
    out("vcs: committed rev "); out_u32((u32)nv);
    out("  msg: "); outln(msg);
}
static void tv_log(void) {
    char buf[1024], pth[128];
    int n;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    resolve_path(TV_LOG, pth, (int)sizeof(pth));
    n = tv_load_shards(pth, buf, sizeof(buf));
    if (n < 0) n = 0;
    buf[n] = 0;
    if (n == 0) { outln("vcs: no commits yet"); return; }
    outln("----- vcs history -----");
    {
        /* a2: semantic coloring - rev cyan, time gray, msg white */
        char *p = buf;
        while (*p) {
            char *nl = my_strchr(p, '\n');
            char *b1 = my_strchr(p, '|');
            char *b2 = b1 ? my_strchr(b1 + 1, '|') : 0;
            char *end = nl ? nl : p + my_strlen(p);
            char *q;
            set_color(11);
            for (q = p; q != (b1 ? b1 : end); q++) api->putc(*q);
            if (b1) {
                api->putc('|');
                set_color(8);
                for (q = b1 + 1; q != (b2 ? b2 : end); q++) api->putc(*q);
                if (b2) {
                    api->putc('|');
                    set_color(15);
                    for (q = b2 + 1; q != end; q++) api->putc(*q);
                }
            }
            outln("");
            if (nl) p = nl + 1; else break;
        }
    }
    outln("------------------------");
}
static void tv_status(void) {
    char dir[128], num[3];
    int head, nv, i, changes = 0;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    tv_load_index();
    head = tv_get_head();
    nv = head;
    my_strcpy(dir, TV_DIR "/REV");
    num[0] = (char)('0' + nv / 10);
    num[1] = (char)('0' + nv % 10);
    num[2] = 0;
    my_strcat(dir, num);
    {
        char pth[128];
        resolve_path(dir, pth, (int)sizeof(pth));
        out("vcs: current rev: "); out_u32((u32)head); outln("");
    }
    outln("vcs status:");
    for (i = 0; i < tv_n; i++) {
        char base[160], fnum[3], fp[128], pth[128];
        int ws, wn, sn;
        my_strcpy(base, dir);
        my_strcat(base, "/F");
        fnum[0] = (char)('0' + i / 10);
        fnum[1] = (char)('0' + i % 10);
        fnum[2] = 0;
        my_strcat(base, fnum);
        resolve_path(tv_names[i], fp, (int)sizeof(fp));
        resolve_path(base, pth, (int)sizeof(pth));
        ws = api->fsize(fp);
        if (ws < 0) { out("  [MISSING] "); outln(tv_names[i]); changes++; continue; }
        wn = api->cat(fp, tv_wbuf, 8192);
        sn = tv_load_shards(pth, tv_sbbuf, 8192);
        if (sn < 0) { out("  [NEW] "); outln(tv_names[i]); changes++; continue; }
        if (wn != sn || (wn > 0 && my_memcmp(tv_wbuf, tv_sbbuf, (u32)wn) != 0)) {
            out("  [MODIFIED] "); outln(tv_names[i]);
            changes++;
        } else {
            out("  [SAME] "); outln(tv_names[i]);
        }
    }
    if (changes == 0) outln("  no changes");
}
static void tv_diff(const char *name) {
    char dir[128], num[3], base[160], fnum[3], fp[128], pth[128];
    int head, i, found = -1, wn, sn;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    if (!name[0]) { outln("usage: vcs diff <file>"); return; }
    tv_load_index();
    head = tv_get_head();
    my_strcpy(dir, TV_DIR "/REV");
    num[0] = (char)('0' + head / 10);
    num[1] = (char)('0' + head % 10);
    num[2] = 0;
    my_strcat(dir, num);
    for (i = 0; i < tv_n; i++)
        if (my_strcmp(tv_names[i], name) == 0) { found = i; break; }
    if (found < 0) { out("vcs: file not tracked: "); outln(name); return; }
    my_strcpy(base, dir);
    my_strcat(base, "/F");
    fnum[0] = (char)('0' + found / 10);
    fnum[1] = (char)('0' + found % 10);
    fnum[2] = 0;
    my_strcat(base, fnum);
    resolve_path(name, fp, (int)sizeof(fp));
    resolve_path(base, pth, (int)sizeof(pth));
    wn = api->cat(fp, tv_wbuf, 8192);
    if (wn < 0) { out("vcs: cannot read worktree: "); outln(name); return; }
    sn = tv_load_shards(pth, tv_sbbuf, 8192);
    if (sn < 0) { outln("vcs: snapshot missing"); return; }
    out("vcs diff (worktree vs rev");
    out_u32((u32)head); outln("):");
    {
        char *wa[256], *sa[256];
        u32 wl[256], sl[256];
        u32 wn2 = 0, sn2 = 0;
        {
            char *p = tv_wbuf;
            while (*p && wn2 < 256) {
                u32 l = 0;
                wa[wn2] = p;
                while (*p && *p != '\n') { p++; l++; }
                wl[wn2] = l;
                wn2++;
                if (*p == '\n') p++;
            }
            p = tv_sbbuf;
            while (*p && sn2 < 256) {
                u32 l = 0;
                sa[sn2] = p;
                while (*p && *p != '\n') { p++; l++; }
                sl[sn2] = l;
                sn2++;
                if (*p == '\n') p++;
            }
        }
        if (wn2 == sn2) {
            for (i = 0; i < (int)wn2; i++) {
                if (wl[i] != sl[i] || my_memcmp(wa[i], sa[i], wl[i]) != 0) {
                    u32 k;
                    api->set_fg(COL_YELLOW);
                    out("~ ");
                    for (k = 0; k < wl[i]; k++) api->putc(wa[i][k]);
                    out(" | ");
                    for (k = 0; k < sl[i]; k++) api->putc(sa[i][k]);
                    outln("");
                    api->set_fg(COL_LGRAY);
                }
            }
        } else {
            u32 k;
            for (i = 0; i < (int)wn2; i++) {
                api->set_fg(COL_GREEN);
                out("> ");
                for (k = 0; k < wl[i]; k++) api->putc(wa[i][k]);
                outln("");
            }
            api->set_fg(COL_LGRAY);
            for (i = 0; i < (int)sn2; i++) {
                api->set_fg(COL_LRED);
                out("< ");
                for (k = 0; k < sl[i]; k++) api->putc(sa[i][k]);
                outln("");
            }
            api->set_fg(COL_LGRAY);
        }
    }
    outln("----- end vcs diff -----");
}
static void tv_checkout(int ver) {
    char dir[128], num[3], probe[128];
    int head, i;
    if (tv_get_head() < 0) { outln("vcs: not initialized, run vcs init first"); return; }
    head = tv_get_head();
    if (ver > head) { out("vcs: rev not exist (current "); out_u32((u32)head); outln(")"); return; }
    tv_load_index();
    my_strcpy(dir, TV_DIR "/REV");
    num[0] = (char)('0' + ver / 10);
    num[1] = (char)('0' + ver % 10);
    num[2] = 0;
    my_strcat(dir, num);
    {
        char pth[128];
        my_strcpy(probe, dir);
        my_strcat(probe, "/F00.A00");
        resolve_path(probe, pth, (int)sizeof(pth));
        if (!api->file_exists(pth)) {
            out("vcs: rev "); out_u32((u32)ver); outln(" has no snapshot");
            return;
        }
    }
    for (i = 0; i < tv_n; i++) {
        char base[160], fnum[3], pth[128], fp[128];
        int sn;
        my_strcpy(base, dir);
        my_strcat(base, "/F");
        fnum[0] = (char)('0' + i / 10);
        fnum[1] = (char)('0' + i % 10);
        fnum[2] = 0;
        my_strcat(base, fnum);
        resolve_path(base, pth, (int)sizeof(pth));
        sn = tv_load_shards(pth, tv_sbbuf, 8192);
        if (sn < 0) continue;
        resolve_path(tv_names[i], fp, (int)sizeof(fp));
        tv_save_shards(fp, tv_sbbuf, (u32)sn);
        out("vcs: restored: "); outln(tv_names[i]);
    }
    out("vcs: checkout rev "); out_u32((u32)ver); outln(" done");
}
static void cmd_vcs(const char *arg) {
    char av[4][40];
    int ac = split_args(arg, av, 4);
    const char *cmd = ac > 0 ? av[0] : "";
    if (ac == 0) {
        outln("usage: vcs init|add F|commit MSG|log|status|diff F|checkout NN");
        return;
    }
    if (my_strcmp(cmd, "init") == 0 || my_strcmp(cmd, "i") == 0) tv_init();
    else if (my_strcmp(cmd, "add") == 0 || my_strcmp(cmd, "a") == 0) tv_add(ac > 1 ? av[1] : "");
    else if (my_strcmp(cmd, "commit") == 0 || my_strcmp(cmd, "c") == 0) tv_commit(ac > 1 ? av[1] : "");
    else if (my_strcmp(cmd, "log") == 0 || my_strcmp(cmd, "l") == 0) tv_log();
    else if (my_strcmp(cmd, "status") == 0 || my_strcmp(cmd, "s") == 0) tv_status();
    else if (my_strcmp(cmd, "diff") == 0 || my_strcmp(cmd, "d") == 0) tv_diff(ac > 1 ? av[1] : "");
    else if (my_strcmp(cmd, "checkout") == 0 || my_strcmp(cmd, "co") == 0) tv_checkout(ac > 1 ? my_atoi(av[1]) : 0);
    else { out("vcs: unknown cmd: "); outln(cmd); }
}


/* ---------- cedbg (tc_) CE inspector: hdr/hex/dis/str/mem/sym/self ---------- */
#define TC_BASE 0x02000000u
#define KB_ESC 27
static char tc_file[64];
static u8 tc_data[8192];
static u32 tc_size = 0;
static u32 tc_loaded = 0;

static int tc_load(const char *path) {
    int n = api->cat(path, (char *)tc_data, sizeof(tc_data));
    if (n < 0) { out("cedbg: cannot read: "); outln(path); return 0; }
    if (n > (int)sizeof(tc_data)) n = (int)sizeof(tc_data);
    tc_size = (u32)n;
    tc_loaded = 1;
    return 1;
}
static void tc_put8(u8 v) {
    static const char hd[] = "0123456789abcdef";
    api->putc(hd[v >> 4]);
    api->putc(hd[v & 15]);
}
static void tc_put32(u32 v) {
    tc_put8((u8)(v >> 24)); tc_put8((u8)(v >> 16));
    tc_put8((u8)(v >> 8)); tc_put8((u8)v);
}
static void tc_hdr(void) {
    u32 magic, entry, bss, apioff, isz;
    if (tc_size < 20) { outln("cedbg: file too small, not a valid CE01 image"); return; }
    magic = (u32)tc_data[0] | ((u32)tc_data[1] << 8) | ((u32)tc_data[2] << 16) | ((u32)tc_data[3] << 24);
    if (magic != 0x31304543u) { out("cedbg: not CE01 (magic=0x"); tc_put32(magic); outln(")"); return; }
    entry = (u32)tc_data[4] | ((u32)tc_data[5] << 8) | ((u32)tc_data[6] << 16) | ((u32)tc_data[7] << 24);
    bss   = (u32)tc_data[8] | ((u32)tc_data[9] << 8) | ((u32)tc_data[10] << 16) | ((u32)tc_data[11] << 24);
    apioff = (u32)tc_data[12] | ((u32)tc_data[13] << 8) | ((u32)tc_data[14] << 16) | ((u32)tc_data[15] << 24);
    isz   = (u32)tc_data[16] | ((u32)tc_data[17] << 8) | ((u32)tc_data[18] << 16) | ((u32)tc_data[19] << 24);
    outln("----- CE01 header -----");
    out("magic      : CE01 (hex "); tc_put32(0x31304543u); outln(")");
    out("entry      : 0x"); tc_put32(entry); out("  ("); out_u32(entry); outln(")");
    out("bss_size   : 0x"); tc_put32(bss); out("  ("); out_u32(bss); outln(" bytes bss)");
    out("api_offset : 0x"); tc_put32(apioff); out("  (base+off = 0x"); tc_put32(TC_BASE + apioff); outln(")");
    out("image_size : 0x"); tc_put32(isz); out("  ("); out_u32(isz); outln(" bytes)");
    out("file size   : "); out_u32(tc_size); outln(" bytes (20 hdr + image)");
    if (isz != tc_size - 20) outln("note: image_size != file size - 20!");
    outln("-----------------");
}
static void tc_hex(u32 off, u32 len) {
    u32 i = 0, line = 0;
    if (!tc_loaded) { outln("cedbg: specify a file first"); return; }
    if (off >= tc_size) { outln("cedbg: offset out of range"); return; }
    if (off + len > tc_size) len = tc_size - off;
    while (i < len) {
        u32 j;
        tc_put32(off + i);
        api->putc(' ');
        for (j = 0; j < 16; j++) {
            if (i + j < len) tc_put8(tc_data[off + i + j]);
            else { api->putc(' '); api->putc(' '); }
            api->putc(' ');
        }
        api->putc(' ');
        for (j = 0; j < 16 && i + j < len; j++) {
            u8 c = tc_data[off + i + j];
            api->putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        outln("");
        i += 16;
        line++;
        if (line >= 20 && i < len) {
            out("--- any key to continue, Esc exits (left "); out_u32(len - i); outln(" bytes) ---");
            if (api->getch() == KB_ESC) return;
            line = 0;
        }
    }
    outln("----- dump end -----");
}
static const char *tc_reg8[] = { "al","cl","dl","bl","ah","ch","dh","bh" };
static const char *tc_reg16[] = { "ax","cx","dx","bx","sp","bp","si","di" };
static const char *tc_reg32[] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
struct tc_dis { char *buf; u32 len; };
static void tc_dput(struct tc_dis *o, const char *s) { while (*s && o->len < 120) o->buf[o->len++] = *s++; }
static void tc_dputc(struct tc_dis *o, char c) { if (o->len < 120) o->buf[o->len++] = c; }
static void tc_dputh(struct tc_dis *o, u32 v) {
    static const char hd[] = "0123456789abcdef";
    char tmp[10];
    int i;
    for (i = 7; i >= 0; i--) { tmp[i] = hd[v & 15]; v >>= 4; }
    tmp[8] = 0;
    tc_dput(o, "0x");
    tc_dput(o, tmp);
}
static void tc_modrm(struct tc_dis *o, u8 modrm, const u8 *p, u32 end, int w, u32 *consumed) {
    u8 mod = modrm >> 6, rm = modrm & 7;
    const char **regs = w ? tc_reg32 : tc_reg16;
    *consumed = 0;
    if (mod == 3) { tc_dput(o, regs[rm]); return; }
    if (rm == 4) {
        u8 sib = 0, scale, idx, base;
        if (p + 1 < (const u8 *)tc_data + tc_size && (u32)(p - tc_data) + 1 < tc_size) sib = p[1];
        scale = sib >> 6; idx = (sib >> 3) & 7; base = sib & 7;
        tc_dputc(o, '[');
        if (mod == 0 && base == 5) tc_dput(o, "disp32");
        else tc_dput(o, regs[base]);
        if (idx != 4) {
            tc_dputc(o, '+');
            tc_dput(o, regs[idx]);
            if (scale) {
                tc_dputc(o, '*');
                tc_dputc(o, (char)('1' + (scale == 1 ? 0 : scale)));
            }
        }
        *consumed = 1;
        if (mod == 1) { tc_dputc(o, '+'); tc_dputh(o, p[*consumed]); *consumed += 1; }
        else if (mod == 2) {
            u32 d = (u32)p[*consumed] | ((u32)p[*consumed+1] << 8) | ((u32)p[*consumed+2] << 16) | ((u32)p[*consumed+3] << 24);
            tc_dputc(o, '+'); tc_dputh(o, d); *consumed += 4;
        }
        tc_dputc(o, ']');
        return;
    }
    if (mod == 0 && rm == 5) {
        u32 d = (u32)p[1] | ((u32)p[2] << 8) | ((u32)p[3] << 16) | ((u32)p[4] << 24);
        tc_dput(o, "["); tc_dputh(o, d); tc_dputc(o, ']');
        *consumed = 4;
        return;
    }
    tc_dputc(o, '[');
    tc_dput(o, regs[rm]);
    if (mod == 1) {
        s32 d = (s8)p[1];
        if (d >= 0) { tc_dputc(o, '+'); tc_dputh(o, (u32)d); }
        else { tc_dputc(o, '-'); tc_dputh(o, (u32)(-d)); }
        *consumed = 1;
    } else if (mod == 2) {
        s32 d = (s32)((u32)p[1] | ((u32)p[2] << 8) | ((u32)p[3] << 16) | ((u32)p[4] << 24));
        if (d >= 0) { tc_dputc(o, '+'); tc_dputh(o, (u32)d); }
        else { tc_dputc(o, '-'); tc_dputh(o, (u32)(-d)); }
        *consumed = 4;
    }
    tc_dputc(o, ']');
}
static u32 tc_disone(u32 off, struct tc_dis *o) {
    u8 op = tc_data[off];
    u32 end = tc_size;
    if (op == 0x0F && off + 1 < end) {
        u8 op2 = tc_data[off + 1];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            static const char *jccn[] = { "jo","jno","jb","jae","je","jne","jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg" };
            u32 rel = (u32)tc_data[off+2] | ((u32)tc_data[off+3] << 8) | ((u32)tc_data[off+4] << 16) | ((u32)tc_data[off+5] << 24);
            tc_dput(o, jccn[op2 - 0x80]);
            tc_dputc(o, ' ');
            tc_dputh(o, TC_BASE + off + 6 + rel);
            return off + 6;
        }
        tc_dput(o, "db 0x0f");
        return off + 1;
    }
    switch (op) {
    case 0x90: tc_dput(o, "nop"); return off + 1;
    case 0xC3: tc_dput(o, "ret"); return off + 1;
    case 0xC9: tc_dput(o, "leave"); return off + 1;
    case 0xC2: case 0xCA: {
        u16 imm = (u16)(tc_data[off+1] | (tc_data[off+2] << 8));
        tc_dput(o, op == 0xC2 ? "ret " : "retf ");
        tc_dputh(o, imm);
        return off + 3;
    }
    case 0xCD: tc_dput(o, "int "); tc_dputh(o, tc_data[off+1]); return off + 2;
    case 0xE8: {
        s32 rel = (s32)((u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24));
        tc_dput(o, "call "); tc_dputh(o, TC_BASE + off + 5 + rel); return off + 5;
    }
    case 0xE9: {
        s32 rel = (s32)((u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24));
        tc_dput(o, "jmp "); tc_dputh(o, TC_BASE + off + 5 + rel); return off + 5;
    }
    case 0xEB: { s8 rel = (s8)tc_data[off+1]; tc_dput(o, "jmp "); tc_dputh(o, TC_BASE + off + 2 + rel); return off + 2; }
    default:
        if (op >= 0x70 && op <= 0x7F) {
            static const char *jccn[] = { "jo","jno","jb","jae","je","jne","jbe","ja","js","jns","jp","jnp","jl","jge","jle","jg" };
            s8 rel = (s8)tc_data[off+1];
            tc_dput(o, jccn[op - 0x70]); tc_dputc(o, ' ');
            tc_dputh(o, TC_BASE + off + 2 + rel);
            return off + 2;
        }
        if (op >= 0x40 && op <= 0x4F) {
            tc_dput(o, (op & 8) ? "dec " : "inc "); tc_dput(o, tc_reg32[op & 7]); return off + 1;
        }
        if (op >= 0x50 && op <= 0x57) { tc_dput(o, "push "); tc_dput(o, tc_reg32[op & 7]); return off + 1; }
        if (op >= 0x58 && op <= 0x5F) { tc_dput(o, "pop "); tc_dput(o, tc_reg32[op & 7]); return off + 1; }
        if (op == 0x68) {
            u32 imm = (u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24);
            tc_dput(o, "push "); tc_dputh(o, imm); return off + 5;
        }
        if (op == 0x6A) { s8 imm = (s8)tc_data[off+1]; tc_dput(o, "push "); tc_dputh(o, (u32)imm); return off + 2; }
        if (op >= 0xB0 && op <= 0xB7) {
            tc_dput(o, "mov "); tc_dput(o, tc_reg8[op & 7]); tc_dputc(o, ',');
            tc_dputh(o, tc_data[off+1]); return off + 2;
        }
        if (op >= 0xB8 && op <= 0xBF) {
            u32 imm = (u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24);
            tc_dput(o, "mov "); tc_dput(o, tc_reg32[op & 7]); tc_dputc(o, ',');
            tc_dputh(o, imm); return off + 5;
        }
        if (op >= 0x80 && op <= 0x83) {
            static const char *ops[] = { "add","or","adc","sbb","and","sub","xor","cmp" };
            u8 modrm = tc_data[off+1];
            u8 sub = (modrm >> 3) & 7;
            int w = (op & 2) ? 1 : 0;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, w, &used);
            o->len += tmp.len;
            tc_dput(o, ops[sub]);
            tc_dputc(o, ' ');
            tc_dputc(o, ',');
            if (op == 0x80 || op == 0x81) {
                u32 imm = (u32)tc_data[off+2+used] | ((u32)tc_data[off+3+used] << 8) | ((u32)tc_data[off+4+used] << 16) | ((u32)tc_data[off+5+used] << 24);
                tc_dputh(o, imm);
                return off + 6 + used;
            } else {
                s8 imm = (s8)tc_data[off+2+used];
                tc_dputh(o, (u32)imm);
                return off + 3 + used;
            }
        }
        if (op >= 0x88 && op <= 0x8B) {
            u8 modrm = tc_data[off+1];
            int w = op & 1;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, w, &used);
            if (op == 0x8B || op == 0x89) {
                tc_dput(o, "mov ");
                if (op == 0x8B) { tc_dput(o, tc_reg32[(modrm >> 3) & 7]); tc_dputc(o, ','); o->len += tmp.len; }
                else { o->len += tmp.len; tc_dputc(o, ','); tc_dput(o, tc_reg32[(modrm >> 3) & 7]); }
            } else {
                tc_dput(o, "mov ");
                if (op == 0x8A) { tc_dput(o, tc_reg8[(modrm >> 3) & 7]); tc_dputc(o, ','); o->len += tmp.len; }
                else { o->len += tmp.len; tc_dputc(o, ','); tc_dput(o, tc_reg8[(modrm >> 3) & 7]); }
            }
            return off + 2 + used;
        }
        if (op == 0x8D) {
            u8 modrm = tc_data[off+1];
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, 1, &used);
            tc_dput(o, "lea ");
            tc_dput(o, tc_reg32[(modrm >> 3) & 7]);
            tc_dputc(o, ',');
            o->len += tmp.len;
            return off + 2 + used;
        }
        if (op <= 0x3F) {
            static const char *ops[] = { "add","or","adc","sbb","and","sub","xor","cmp" };
            u8 modrm = tc_data[off+1];
            int w = op & 1;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, w, &used);
            tc_dput(o, ops[(op >> 3) & 7]);
            tc_dputc(o, ' ');
            if ((op & 0xC0) == 0x00) { o->len += tmp.len; tc_dputc(o, ','); tc_dput(o, w ? tc_reg32[(modrm >> 3) & 7] : tc_reg8[(modrm >> 3) & 7]); }
            else { tc_dput(o, w ? tc_reg32[(modrm >> 3) & 7] : tc_reg8[(modrm >> 3) & 7]); tc_dputc(o, ','); o->len += tmp.len; }
            return off + 2 + used;
        }
        if (op >= 0xD0 && op <= 0xD3) {
            static const char *ops[] = { "rol","ror","rcl","rcr","shl","shr","sal","sar" };
            u8 modrm = tc_data[off+1];
            u8 sub = (modrm >> 3) & 7;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, 1, &used);
            tc_dput(o, ops[sub]); tc_dputc(o, ' ');
            o->len += tmp.len;
            tc_dputc(o, ',');
            if (op == 0xD2) tc_dput(o, "cl");
            else if (op == 0xD0 || op == 0xD1) tc_dput(o, "1");
            else tc_dputh(o, tc_data[off+2+used]);
            return off + 2 + used + ((op == 0xD3) ? 1 : 0);
        }
        if (op == 0xC7) {
            u8 modrm = tc_data[off+1];
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, 1, &used);
            tc_dput(o, "mov "); o->len += tmp.len; tc_dputc(o, ',');
            {
                u32 imm = (u32)tc_data[off+2+used] | ((u32)tc_data[off+3+used] << 8) | ((u32)tc_data[off+4+used] << 16) | ((u32)tc_data[off+5+used] << 24);
                tc_dputh(o, imm);
            }
            return off + 6 + used;
        }
        if (op >= 0xC0 && op <= 0xC1) {
            static const char *sops[] = { "rol","ror","rcl","rcr","shl","shr","sal","sar" };
            u8 modrm = tc_data[off+1];
            u8 sub = (modrm >> 3) & 7;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, op == 0xC1, &used);
            tc_dput(o, sops[sub]); tc_dputc(o, ' ');
            o->len += tmp.len;
            tc_dputc(o, ',');
            tc_dputh(o, tc_data[off+2+used]);
            return off + 3 + used;
        }
        if (op == 0x84 || op == 0x85) {
            u8 modrm = tc_data[off+1];
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, op == 0x85, &used);
            tc_dput(o, "test "); o->len += tmp.len; tc_dputc(o, ',');
            tc_dput(o, (op == 0x85) ? tc_reg32[(modrm >> 3) & 7] : tc_reg8[(modrm >> 3) & 7]);
            return off + 2 + used;
        }
        if (op == 0xA8 || op == 0xA9) {
            tc_dput(o, "test ");
            tc_dput(o, op == 0xA8 ? "al," : "eax,");
            if (op == 0xA8) { tc_dputh(o, tc_data[off+1]); return off + 2; }
            {
                u32 imm = (u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24);
                tc_dputh(o, imm);
            }
            return off + 5;
        }
        if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
            u32 addr = (u32)tc_data[off+1] | ((u32)tc_data[off+2] << 8) | ((u32)tc_data[off+3] << 16) | ((u32)tc_data[off+4] << 24);
            tc_dput(o, "mov ");
            if (op == 0xA0 || op == 0xA1) { tc_dput(o, (op == 0xA0) ? "al,[" : "eax,["); }
            else { tc_dput(o, "["); tc_dputh(o, addr); tc_dput(o, "],"); tc_dput(o, (op == 0xA2) ? "al" : "eax"); return off + 5; }
            tc_dputh(o, addr);
            tc_dputc(o, ']');
            return off + 5;
        }
        if (op == 0xF6 || op == 0xF7) {
            static const char *fops[] = { "test","test","not","neg","mul","imul","div","idiv" };
            u8 modrm = tc_data[off+1];
            u8 sub = (modrm >> 3) & 7;
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, op == 0xF7, &used);
            tc_dput(o, fops[sub]); tc_dputc(o, ' ');
            o->len += tmp.len;
            {
                u32 adv = 2 + used;
                if (sub == 0) {
                    tc_dputc(o, ',');
                    if (op == 0xF6) { tc_dputh(o, tc_data[off+2+used]); adv += 1; }
                    else {
                        u32 imm = (u32)tc_data[off+2+used] | ((u32)tc_data[off+3+used] << 8) | ((u32)tc_data[off+4+used] << 16) | ((u32)tc_data[off+5+used] << 24);
                        tc_dputh(o, imm); adv += 4;
                    }
                }
                return off + adv;
            }
        }
        if (op == 0x69 || op == 0x6B) {
            u8 modrm = tc_data[off+1];
            u32 used = 0;
            struct tc_dis tmp;
            tmp.buf = o->buf + o->len; tmp.len = 0;
            tc_modrm(&tmp, modrm, tc_data + off + 2, end, 1, &used);
            tc_dput(o, "imul ");
            tc_dput(o, tc_reg32[(modrm >> 3) & 7]); tc_dputc(o, ',');
            o->len += tmp.len;
            tc_dputc(o, ',');
            if (op == 0x6B) { s8 imm = (s8)tc_data[off+2+used]; tc_dputh(o, (u32)imm); return off + 3 + used; }
            {
                u32 imm = (u32)tc_data[off+2+used] | ((u32)tc_data[off+3+used] << 8) | ((u32)tc_data[off+4+used] << 16) | ((u32)tc_data[off+5+used] << 24);
                tc_dputh(o, imm);
            }
            return off + 6 + used;
        }
        if (op == 0xE0 || op == 0xE1 || op == 0xE2 || op == 0xE3) {
            s8 rel = (s8)tc_data[off+1];
            tc_dput(o, op == 0xE0 ? "loopnz " : op == 0xE1 ? "loopz " : op == 0xE2 ? "loop " : "jcxz ");
            tc_dputh(o, TC_BASE + off + 2 + rel);
            return off + 2;
        }
        if (op == 0xA4) { tc_dput(o, "movsb"); return off + 1; }
        if (op == 0xA5) { tc_dput(o, "movsd"); return off + 1; }
        if (op == 0xAA) { tc_dput(o, "stosb"); return off + 1; }
        if (op == 0xAB) { tc_dput(o, "stosd"); return off + 1; }
        if (op == 0xAC) { tc_dput(o, "lodsb"); return off + 1; }
        if (op == 0xAD) { tc_dput(o, "lodsd"); return off + 1; }
        if (op == 0xCC) { tc_dput(o, "int3"); return off + 1; }
        if (op == 0xCB) { tc_dput(o, "retf"); return off + 1; }
        if (op == 0xFC) { tc_dput(o, "cld"); return off + 1; }
        if (op == 0xFD) { tc_dput(o, "std"); return off + 1; }
        if (op == 0xF4) { tc_dput(o, "hlt"); return off + 1; }
        if (op == 0xF8) { tc_dput(o, "clc"); return off + 1; }
        if (op == 0xF9) { tc_dput(o, "stc"); return off + 1; }
        tc_dput(o, "db 0x");
        {
            static const char hd[] = "0123456789abcdef";
            char b[3];
            b[0] = hd[op >> 4]; b[1] = hd[op & 15]; b[2] = 0;
            tc_dput(o, b);
        }
        return off + 1;
    }
}
static void tc_dis(u32 off, u32 len) {
    u32 i, done = 0, lines = 0, unknown = 0;
    if (!tc_loaded) { outln("cedbg: specify a file first"); return; }
    if (off >= tc_size) { outln("cedbg: offset out of range"); return; }
    if (off + len > tc_size) len = tc_size - off;
    if (len > 256) len = 256;
    i = off;
    while (i < off + len && i < tc_size) {
        char line[140];
        struct tc_dis o;
        u32 k, next;
        o.buf = line; o.len = 0;
        next = tc_disone(i, &o);
        if (next <= i || next > tc_size) { unknown++; i++; continue; }
        tc_put32(TC_BASE + i);
        api->putc(':'); api->putc(' ');
        for (k = 0; k < next - i && k < 8; k++) { tc_put8(tc_data[i + k]); api->putc(' '); }
        for (k = next - i; k < 8; k++) { api->putc(' '); api->putc(' '); api->putc(' '); }
        for (k = 0; k < o.len; k++) api->putc(line[k]);
        outln("");
        i = next;
        done++;
        lines++;
        if (lines >= 20 && i < off + len) {
            out("--- any key to continue, Esc exits ---");
            if (api->getch() == KB_ESC) break;
            lines = 0;
        }
    }
    out_u32(done); outln(" instructions");
    if (unknown) { out_u32(unknown); outln(" bytes undecoded (db fallback)"); }
}
static void tc_str(void) {
    u32 i = 0, found = 0;
    if (!tc_loaded) { outln("cedbg: specify a file first"); return; }
    while (i < tc_size) {
        u32 start = i;
        while (i < tc_size && tc_data[i] >= 32 && tc_data[i] < 127) i++;
        if (i - start >= 4) {
            u32 k;
            tc_put32(start);
            api->putc(':'); api->putc(' ');
            for (k = start; k < i; k++) api->putc((char)tc_data[k]);
            outln("");
            found++;
        }
        i++;
    }
    out_u32(found); outln(" strings");
}
static void tc_mem(void) {
    u32 bss, apioff, isz;
    if (tc_size < 20) { outln("cedbg: file too small, not a valid CE01 image"); return; }
    bss = (u32)tc_data[8] | ((u32)tc_data[9] << 8) | ((u32)tc_data[10] << 16) | ((u32)tc_data[11] << 24);
    apioff = (u32)tc_data[12] | ((u32)tc_data[13] << 8) | ((u32)tc_data[14] << 16) | ((u32)tc_data[15] << 24);
    isz = (u32)tc_data[16] | ((u32)tc_data[17] << 8) | ((u32)tc_data[18] << 16) | ((u32)tc_data[19] << 24);
    outln("----- memory estimate -----");
    out("load base        : 0x02000000\n");
    out("image body       : "); out_u32(isz); outln(" bytes (code+rodata+data)");
    out(".bss zero seg    : "); out_u32(bss); outln(" bytes");
    out("kernel stack    : 8 KiB (kernel-provided)");
    outln("");
    out("heap             : 64 KiB static pool (alloc/free)");
    outln("");
    out("static total    : "); out_u32(isz + bss); outln(" bytes (image+bss)");
    out("total need(est) : "); out_u32(isz + bss + 8192 + 65536); outln(" bytes (+stack+heap)");
    out("API table addr  : 0x");
    tc_put32(TC_BASE + apioff);
    outln("");
    outln("-------------------");
}
static void tc_sym(void) {
    char elfpath[96];
    char ebuf[8192];
    int n;
    if (!tc_loaded) { outln("cedbg: specify a file first"); return; }
    my_strcpy(elfpath, tc_file);
    {
        char *dot = my_strrchr(elfpath, '.');
        if (dot && (my_strcmp(dot, ".ce") == 0 || my_strcmp(dot, ".CE") == 0)) {
            dot[1] = 'E'; dot[2] = 'L'; dot[3] = 'F'; dot[4] = 0;
        } else {
            my_strcat(elfpath, ".ELF");
        }
    }
    if (!api->file_exists(elfpath)) {
        out("cedbg: no symbol file: "); outln(elfpath);
        outln("hint: put <name>.elf next to the .ce to parse");
        return;
    }
    n = api->cat(elfpath, ebuf, sizeof(ebuf));
    if (n < 0) { outln("cedbg: ELF read failed"); return; }
    if (n < 52 || !(ebuf[0] == 0x7F && ebuf[1] == 'E' && ebuf[2] == 'L' && ebuf[3] == 'F')) {
        outln("cedbg: not a valid ELF32 file");
        return;
    }
    {
        u32 shoff = (u32)((u8)ebuf[0x20] | ((u8)ebuf[0x21] << 8) | ((u8)ebuf[0x22] << 16) | ((u8)ebuf[0x23] << 24));
        u16 shentsz = (u16)((u8)ebuf[0x2E] | ((u8)ebuf[0x2F] << 8));
        u16 shnum = (u16)((u8)ebuf[0x30] | ((u8)ebuf[0x31] << 8));
        u32 found = 0;
        u16 si;
        if (shentsz < 40 || shnum == 0 || shoff == 0) { outln("cedbg: invalid section table"); return; }
        out("cedbg symbol file: "); outln(elfpath);
        outln("----- symbols (.symtab) -----");
        for (si = 0; si < shnum; si++) {
            u32 so = shoff + (u32)si * shentsz;
            u32 type, soff, ssz, entsz, k;
            if (so + 40 > (u32)n) break;
            type = (u32)((u8)ebuf[so+4] | ((u8)ebuf[so+5] << 8) | ((u8)ebuf[so+6] << 16) | ((u8)ebuf[so+7] << 24));
            if (type != 2) continue;
            soff = (u32)((u8)ebuf[so+16] | ((u8)ebuf[so+17] << 8) | ((u8)ebuf[so+18] << 16) | ((u8)ebuf[so+19] << 24));
            ssz  = (u32)((u8)ebuf[so+20] | ((u8)ebuf[so+21] << 8) | ((u8)ebuf[so+22] << 16) | ((u8)ebuf[so+23] << 24));
            entsz = (u32)((u8)ebuf[so+36] | ((u8)ebuf[so+37] << 8) | ((u8)ebuf[so+38] << 16) | ((u8)ebuf[so+39] << 24));
            if (entsz < 16) entsz = 16;
            for (k = 0; k + 16 <= ssz; k += entsz) {
                u32 eo = soff + k;
                u32 st_name, st_value;
                u8 st_info;
                u16 st_shndx;
                u8 bind;
                char nm[64];
                u32 mi, nameo;
                if (eo + 16 > (u32)n) break;
                st_name = (u32)((u8)ebuf[eo] | ((u8)ebuf[eo+1] << 8) | ((u8)ebuf[eo+2] << 16) | ((u8)ebuf[eo+3] << 24));
                st_value = (u32)((u8)ebuf[eo+4] | ((u8)ebuf[eo+5] << 8) | ((u8)ebuf[eo+6] << 16) | ((u8)ebuf[eo+7] << 24));
                st_info = (u8)ebuf[eo+12];
                st_shndx = (u16)((u8)ebuf[eo+14] | ((u8)ebuf[eo+15] << 8));
                bind = st_info >> 4;
                if (st_shndx == 0 || st_value == 0) continue;
                if (bind != 0 && bind != 1) continue;
                nameo = st_name;
                for (mi = 0; mi < 63 && nameo + mi < (u32)n && ebuf[nameo+mi]; mi++) nm[mi] = ebuf[nameo+mi];
                nm[mi] = 0;
                tc_put32(st_value);
                api->putc(' '); api->putc(' ');
                out(nm);
                outln("");
                found++;
            }
        }
        out_u32(found); outln(" symbols");
    }
}
static void tc_self(void) {
    outln("----- runtime self-check -----");
    if (tc_load("/bin/ocntools.ce")) {
        out("self image: /bin/ocntools.ce (");
        out_u32(tc_size); outln(" bytes)");
        tc_hdr();
    } else {
        outln("cedbg: cannot read self (/bin/ocntools.ce)");
    }
    {
        void *p = api->alloc(32);
        if (p) { outln("heap alloc 32B: OK"); api->free(p); outln("heap free: OK"); }
        else outln("heap alloc: FAILED");
    }
    outln("-------------------");
}
static void cmd_cedbg(const char *arg) {
    char av[6][40];
    int ac = split_args(arg, av, 6);
    const char *sub;
    u32 off = 0, len = 0;
    if (ac == 0) {
        outln("usage: cedbg <file> [hdr|hex OFF LEN|dis OFF LEN|str|mem|sym|self]");
        return;
    }
    if (ac >= 1 && my_strcmp(av[0], "self") == 0) { tc_self(); return; }
    {
        char p[128];
        resolve_existing(av[0], p, (int)sizeof(p));
        my_strcpy(tc_file, p);
        if (!tc_load(p)) return;
    }
    sub = ac >= 2 ? av[1] : "";
    if (ac >= 3) off = (u32)my_atoi(av[2]);
    if (ac >= 4) len = (u32)my_atoi(av[3]);
    if (!sub[0]) {
        out("cedbg file: "); outln(tc_file);
        tc_hdr();
        outln("hint: subcmds hdr/hex/dis/str/mem/sym/self");
        return;
    }
    if (my_strcmp(sub, "hdr") == 0) tc_hdr();
    else if (my_strcmp(sub, "hex") == 0) { if (!len) len = 256; tc_hex(off, len); }
    else if (my_strcmp(sub, "dis") == 0) { if (!len) len = 128; tc_dis(off, len); }
    else if (my_strcmp(sub, "str") == 0) tc_str();
    else if (my_strcmp(sub, "mem") == 0) tc_mem();
    else if (my_strcmp(sub, "sym") == 0) tc_sym();
    else { out("cedbg: unknown subcmd: "); outln(sub); }
}

#include "hexedit_port.c"
#include "fmgr_port.c"

/* hexedit (thex_) - full-screen text/hex editor, sub-loop inside ot */
static void cmd_fmgr(const char *arg) {
    (void)arg;
    tfm_main();
}

static void cmd_hexedit(const char *arg) {
    char av[2][40];
    int ac = split_args(arg, av, 2);
    if (ac > 0) { thex_main(av[0]); }
    else thex_main((const char *)0);
}

static void cmd_unknown(const char *cmd) {
    /* shortcut: a bare file name opens it with its default handler:
       .csc -> run script, .ce -> kernel hint, anything else -> cat */
    if (cmd[0] && !my_strchr(cmd, ' ') && cmd[0] != '/') {
        char p[160];
        resolve_path(cmd, p, sizeof(p));
        if (!api->file_exists(p)) {
            char b[160];
            b[0] = '/'; b[1] = 'b'; b[2] = 'i'; b[3] = 'n'; b[4] = '/';
            my_strcpy(b + 5, cmd);
            if (api->file_exists(b)) my_strcpy(p, b);
            else {
                set_color(12);   /* a2: unified light-red error */
                out("not found: ");
                outln(cmd);
                outln("type 'help' for the command list");
                set_color(15);
                return;
            }
        }
        outln("");
        {
            const char *e = my_strrchr(p, '.');
            if (e && my_strcasecmp(e, ".csc") == 0) { run_script(p); return; }
            if (e && my_strcasecmp(e, ".ce") == 0) { cmd_run(p); return; }
            cmd_cat(p);
        }
        return;
    }
    set_color(12);
    out("ocean: unknown command: ");
    outln(cmd);
    set_color(15);
    outln("type 'help' for the command list");
}

/* ---- command table ---- */
struct cmdtab {
    const char *name;
    void (*fn)(const char *arg);
};
static const struct cmdtab TABLE[] = {
    { "help",    cmd_help },
    { "?",       cmd_help },
    { "clear",   cmd_clear },
    { "echo",    cmd_echo },
    { "about",   cmd_about },
    { "banner",  cmd_banner },
    { "uname",   cmd_uname },
    { "date",    cmd_date },
    { "time",    cmd_date },
    { "color",   cmd_color },
    { "colors",  cmd_colors },
    { "dir",     cmd_dir },
    { "ls",      cmd_dir },
    { "cd",      cmd_cd },
    { "mkdir",   cmd_mkdir },
    { "touch",   cmd_touch },
    { "cat",     cmd_cat },
    { "edit",    cmd_edit },
    { "files",   cmd_files },
    { "cecheck", cmd_cecheck },
    { "sft",     cmd_grep },
    { "grep",    cmd_grep },      /* legacy alias */
    { "vcs",     cmd_vcs },
    { "cedbg",   cmd_cedbg },
    { "edt",     cmd_hexedit },
    { "hexedit", cmd_hexedit },   /* legacy alias */
    { "fm",      cmd_fmgr },
    { "fmgr",    cmd_fmgr },      /* legacy alias */
    { "mem",     cmd_mem },
    { "ping",    cmd_ping },
    { "dns",     cmd_dns },
    { "curl",    cmd_curl },
    { "udp",     cmd_udp },
    { "netinfo", cmd_netinfo },
    { "net",     cmd_netinfo },
    { "csc",     cmd_csc },
    { "run",     cmd_run },
    { "alias",   cmd_alias },
    { "reboot",  cmd_reboot },
    { "hang",    cmd_hang },
    { "format",  cmd_format },
    { "netdiag", cmd_netdiag },
    { "exit",    0 },
    { "quit",    0 },
    { "bye",     0 },
};

static int g_quit = 0;

/* ---- v1.3 modern input: history + tab completion + alias ----
   key map (probed on VM): UP=0x100 DOWN=0x101 LEFT=0x102 RIGHT=0x103
   TAB=0x09 BS=0x08 ENTER=0x0A.  No gotoxy API -> append-style editing:
   BS deletes last char, UP/DOWN walk history, TAB completes token.  */
#define HIST_MAX 16
#define HIST_LEN 100
#define ALIAS_MAX 8
#define ALIAS_LEN 96
static char g_hist[HIST_MAX][HIST_LEN];
static int g_hist_n = 0;
static int g_hist_pos = -1;
static char g_alias[ALIAS_MAX][ALIAS_LEN];
static int g_alias_n = 0;

static void hist_push(const char *line) {
    int i;
    if (!line[0]) return;
    if (g_hist_n > 0 && my_strcmp(g_hist[g_hist_n - 1], line) == 0) return;
    if (g_hist_n >= HIST_MAX) {
        for (i = 0; i < HIST_MAX - 1; i++) my_strcpy(g_hist[i], g_hist[i + 1]);
        g_hist_n = HIST_MAX - 1;
    }
    my_strcpy(g_hist[g_hist_n], line);
    g_hist_n++;
}

/* v1.3 modern prompt: name(theme color) : cwd(blue) $ */
static void render_prompt(void) {
    set_color(CFG.color_prompt);
    out(CFG.prompt);
    set_color(7);
    out(":");
    set_color(9);
    out(cwd);
    set_color(CFG.color_prompt);
    out("$ ");
    set_color(15);
}

static void line_redraw(const char *buf) {
    char pad[91];
    u32 i;
    api->putc('\r');
    render_prompt();
    out(buf);
    for (i = 0; i < 90; i++) pad[i] = ' ';
    pad[90] = 0;
    out(pad);
    api->putc('\r');
    render_prompt();
    out(buf);
}

/* file-name collector used by tab completion */
struct comp_ctx { char names[48][16]; int n; };
static void comp_cb(int is_dir, const char *name, u32 size, void *ud) {
    struct comp_ctx *c = (struct comp_ctx *)ud;
    (void)is_dir; (void)size;
    if (c->n >= 48) return;
    my_strcpy(c->names[c->n], name);
    c->n++;
}

/* ---- external commands: /bin/*.csc registered at startup ---- */
static void ext_scan(void) {
    struct comp_ctx ctx;
    int i;
    g_ext_n = 0;
    if (!api->fs_ready()) return;
    ctx.n = 0;
    {
        char p[128];
        resolve_path("/bin", p, sizeof(p));
        if (api->ls(p, comp_cb, &ctx) != 0) return;
    }
    for (i = 0; i < ctx.n && g_ext_n < EXT_MAX; i++) {
        const char *nm = ctx.names[i];
        const char *e = my_strrchr(nm, '.');
        if (e && my_strcasecmp(e, ".csc") == 0) {
            u32 L = (u32)(e - nm);
            u32 k2;
            char *dst;
            if (L > 0 && L < 16) {
                dst = g_ext_cmds[g_ext_n];
                for (k2 = 0; k2 < L; k2++) {
                    char c = nm[k2];
                    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    dst[k2] = c;
                }
                dst[L] = 0;
                g_ext_n++;
            }
        }
    }
}

/* resolve NAME.csc in cwd or /bin; returns 1 on success */
static int resolve_ext_script(const char *name, char *out, int outsz) {
    char cand[160];
    my_strcpy(cand, name);
    my_strcat(cand, ".csc");
    return resolve_existing(cand, out, outsz);
}

/* complete the token before the cursor: commands first, then files */
static void line_complete(char *buf, int *len) {
    char *sp = 0;
    const char *tok;
    u32 toklen;
    int i;
    for (i = 0; i < *len; i++) if (buf[i] == ' ') sp = buf + i;
    if (sp) { tok = sp + 1; toklen = (u32)(buf + *len - tok); }
    else    { tok = buf;    toklen = (u32)*len; }
    if (toklen > 60) return;
    if (!sp) {
        /* command-name completion */
        for (i = 0; i < (int)(sizeof(TABLE) / sizeof(TABLE[0])); i++) {
            if (my_strncmp(TABLE[i].name, tok, toklen) == 0 &&
                my_strlen(TABLE[i].name) > toklen) {
                u32 j;
                for (j = toklen; j < my_strlen(TABLE[i].name) && *len < 78; j++)
                    buf[(*len)++] = TABLE[i].name[j];
                buf[*len] = 0;
                return;
            }
        }
        /* external commands */
        for (i = 0; i < g_ext_n; i++) {
            if (my_strncmp(g_ext_cmds[i], tok, toklen) == 0 &&
                my_strlen(g_ext_cmds[i]) > toklen) {
                u32 j;
                for (j = toklen; j < my_strlen(g_ext_cmds[i]) && *len < 78; j++)
                    buf[(*len)++] = g_ext_cmds[i][j];
                buf[*len] = 0;
                return;
            }
        }
    } else {
        /* file completion in cwd (and /bin fallback) */
        struct comp_ctx ctx;
        char p[128];
        int k;
        ctx.n = 0;
        resolve_path(".", p, sizeof(p));
        if (api->ls(p, comp_cb, &ctx) == 0) {
            for (k = 0; k < ctx.n; k++) {
                if (my_strncmp(ctx.names[k], tok, toklen) == 0 &&
                    my_strlen(ctx.names[k]) > toklen) {
                    u32 j;
                    for (j = 0; j < my_strlen(ctx.names[k]) && *len < 78; j++)
                        buf[(*len)++] = ctx.names[k][j];
                    buf[*len] = 0;
                    return;
                }
            }
        }
        ctx.n = 0;
        if (api->ls("/bin", comp_cb, &ctx) == 0) {
            for (k = 0; k < ctx.n; k++) {
                if (my_strncmp(ctx.names[k], tok, toklen) == 0 &&
                    my_strlen(ctx.names[k]) > toklen) {
                    u32 j;
                    for (j = 0; j < my_strlen(ctx.names[k]) && *len < 78; j++)
                        buf[(*len)++] = ctx.names[k][j];
                    buf[*len] = 0;
                    return;
                }
            }
        }
    }
}

/* modern line input; returns 1 on Enter (out = line), 0 on EOF */
static int my_readline(char *out, int max) {
    char buf[80];
    int len = 0;
    my_memset(buf, 0, sizeof(buf));
    line_redraw(buf);
    for (;;) {
        int c = api->getch();
        if (c == '\n' || c == '\r') {
            if (len < max) { my_memcpy(out, buf, (u32)len); out[len] = 0; }
            outln("");   /* force a newline so program output starts on its own line (a2 fix) */
            return 1;
        }
        if (c == 0x08) { /* backspace */
            if (len > 0) { len--; buf[len] = 0; line_redraw(buf); }
            continue;
        }
        if (c == 0x100) { /* up: history back */
            if (g_hist_pos < 0) g_hist_pos = g_hist_n;
            if (g_hist_pos > 0) {
                g_hist_pos--;
                my_strcpy(buf, g_hist[g_hist_pos]);
                len = (int)my_strlen(buf);
                line_redraw(buf);
            }
            continue;
        }
        if (c == 0x101) { /* down: history forward */
            if (g_hist_pos >= 0 && g_hist_pos < g_hist_n - 1) {
                g_hist_pos++;
                my_strcpy(buf, g_hist[g_hist_pos]);
                len = (int)my_strlen(buf);
                line_redraw(buf);
            } else if (g_hist_pos == g_hist_n - 1 || (g_hist_pos < 0 && g_hist_n > 0)) {
                g_hist_pos = -1;
                buf[0] = 0;
                len = 0;
                line_redraw(buf);
            }
            continue;
        }
        if (c == 0x09) { /* tab: complete */
            line_complete(buf, &len);
            line_redraw(buf);
            continue;
        }
        if (c == 0x102 || c == 0x103 || c == 0x1b) continue; /* no gotoxy: ignore */
        if (c >= 0x20 && c <= 0x7e && len < 78) {
            buf[len++] = (char)c;
            buf[len] = 0;
            api->putc((char)c);
        }
    }
}

/* ---- alias ---- */
static void cmd_alias(const char *arg) {
    int i;
    if (!arg[0]) {
        if (g_alias_n == 0) { set_color(8); outln("(no aliases defined)"); set_color(15); return; }
        for (i = 0; i < g_alias_n; i++) {
            /* a2: color the alias name, the '=' and command dimmer */
            char *eq = my_strchr(g_alias[i], '=');
            char *q;
            set_color(11);
            for (q = g_alias[i]; eq && q != eq; q++) api->putc(*q);
            if (!eq) out(g_alias[i]);
            set_color(8);
            out("=");
            set_color(15);
            outln(eq ? eq + 1 : "");
        }
        return;
    }
    {
        char *eq = my_strchr(arg, '=');
        if (eq) {
            u32 namelen = (u32)(eq - arg);
            if (namelen >= 1 && namelen <= 24 && g_alias_n < ALIAS_MAX) {
                char entry[ALIAS_LEN];
                u32 c = 0;
                while (c < namelen && c < 23) { entry[c] = arg[c]; c++; }
                entry[c++] = '=';
                {
                    const char *v = eq + 1;
                    while (*v && c < ALIAS_LEN - 1) entry[c++] = *v++;
                }
                entry[c] = 0;
                for (i = 0; i < g_alias_n; i++)
                    if (my_strncmp(g_alias[i], entry, namelen + 1) == 0) {
                        my_strcpy(g_alias[i], entry);
                        out("alias: ");
                        outln(entry);
                        return;
                    }
                my_strcpy(g_alias[g_alias_n], entry);
                g_alias_n++;
                out("alias: ");
                outln(entry);
                return;
            }
            outln("alias: name too long or too many aliases (max 8)");
            return;
        }
        outln("usage: alias  |  alias NAME=CMD");
    }
}

/* expand a leading alias on a command line (loop-safe) */
static void alias_expand(char *line, int line_sz) {
    int depth;
    (void)line_sz;
    for (depth = 0; depth < 4; depth++) {
        char *sp = my_strchr(line, ' ');
        u32 namelen = sp ? (u32)(sp - line) : my_strlen(line);
        int i;
        for (i = 0; i < g_alias_n; i++) {
            u32 eq = 0;
            while (g_alias[i][eq] && g_alias[i][eq] != '=') eq++;
            if (eq == namelen && my_strncmp(g_alias[i], line, namelen) == 0) {
                char rest[96];
                char newline[160];
                if (sp) my_strcpy(rest, sp + 1);
                else rest[0] = 0;
                my_strcpy(newline, g_alias[i] + eq + 1);
                if (rest[0]) { my_strcat(newline, " "); my_strcat(newline, rest); }
                my_strcpy(line, newline);
                break;
            }
        }
        if (i >= g_alias_n) break;
    }
}

/* execute one command line (used by the prompt loop and startup_cmd) */
static void run_line(char *line) {
    const char *cmd;
    const char *args;
    char *sp;
    int i, found = 0;
    if (line[0] == 0) return;
    alias_expand(line, 160);
    sp = my_strchr(line, ' ');
    if (sp) { *sp = 0; cmd = line; args = sp + 1; }
    else { cmd = line; args = ""; }
    for (i = 0; i < (int)(sizeof(TABLE) / sizeof(TABLE[0])); i++) {
        if (my_strcmp(TABLE[i].name, cmd) == 0) {
            found = 1;
            if (TABLE[i].fn == 0) {
                /* exit family */
                set_color(12);
                outln("bye - returning to the CatOS shell.");
                set_color(15);
                g_quit = 1;
                break;
            }
            TABLE[i].fn(args);
            break;
        }
    }
    if (!found) {
        /* external command: NAME.csc in cwd or /bin, args passed as $1..$9 */
        int x;
        for (x = 0; x < g_ext_n; x++) {
            if (my_strcmp(g_ext_cmds[x], cmd) == 0) {
                char p[160];
                char av2[9][40];
                int ac2;
                char line2[160];
                found = 1;
                my_strcpy(line2, line);
                if (args[0]) { my_strcat(line2, " "); my_strcat(line2, args); }
                ac2 = split_args(line2, av2, 9);
                if (resolve_ext_script(cmd, p, (int)sizeof(p))) {
                    run_script_args(p, ac2 > 1 ? &av2[1] : 0, ac2 > 1 ? ac2 - 1 : 0);
                } else {
                    set_color(12);
                    out("external command missing: ");
                    outln(cmd);
                    set_color(15);
                }
                break;
            }
        }
    }
    if (!found) cmd_unknown(cmd);
}

__attribute__((section(".text.start")))
int ce_main(int argc, char **argv)
{
    int i;
    (void)argc; (void)argv;
    if (!api) { api->write("fatal: no api table\n"); return -1; }

    cfg_load();

    api->cls();
    ext_scan();   /* register /bin/*.csc as external commands */
    cmd_banner("");
    set_color(11);
    outln(CFG.welcome);
    set_color(7);
    out(CFG.info_boot); api->time_str(cwd); outln(cwd);
    set_color(7);
    out(CFG.info_fs);
    if (api->fs_ready()) outln("FAT32 on IDE (mounted)");
    else outln("not mounted (kernel 'format' needed)");
    set_color(7);
    out(CFG.info_net);
    if (api->net_up()) {
        char ip[32];
        api->net_ip(ip);
        out("up, ip ");
        outln(ip);
    } else outln("NIC down (add -device pcnet)");
    set_color(15);
    outln(CFG.hint);
    set_color(15);

    my_strcpy(cwd, "/");

    /* startup commands from the config file, in order */
    for (i = 0; i < CFG.n_startup; i++) {
        char ln[80];
        my_strcpy(ln, CFG.startup[i]);
        run_line(ln);
        outln("");
    }

    while (!g_quit) {
        char line[160];
        render_prompt();
        if (my_readline(line, (int)sizeof(line)) <= 0) continue;
        hist_push(line);
        run_line(line);
        outln("");
    }
    /* NOTE: this kernel's api->exit() does NOT return to the shell -
       it re-launches the same CE image (v0.2 bug, verified with probe).
       A plain return from ce_main goes back to the CatOS shell. */
    return 0;
}
