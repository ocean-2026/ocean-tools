/* game.c - ADoFAI-mini text rhythm game for CatOS CE01.
 * Build: bash build.sh game  -> game.ce
 * All-integer timing, file-level static buffers, PS/2 port polling during play.
 */
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef signed int     s32;

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
    fn_write write; fn_putc putc; fn_cls cls; fn_readline readline;
    fn_time_str time_str; fn_fs_ready fs_ready; fn_ls ls; fn_touch touch;
    fn_mkdir mkdir; fn_cat cat; fn_save save; fn_alloc alloc; fn_free free;
    fn_exit exit;
    fn_print_int print_int; fn_print_hex print_hex; fn_set_fg set_fg;
    fn_delay delay; fn_atoi atoi; fn_strlen strlen; fn_strcmp strcmp;
    fn_strcpy strcpy; fn_getch getch; fn_file_exists file_exists; fn_fsize fsize;
    fn_net_init net_init; fn_net_up net_up; fn_http_get http_get; fn_net_dns net_dns;
    fn_net_udp_send net_udp_send; fn_net_udp_recv net_udp_recv; fn_net_ping net_ping;
    fn_net_ip net_ip;
} ce_api_t;
static ce_api_t *__ce_api __attribute__((section(".ce_api"))) = (void *)0;
#define api __ce_api

#define KB_STATUS 0x64
#define KB_DATA   0x60
#define PIC_IMR   0x21

#define MAX_TILES 256
#define LEADIN_MS 1200

/* color constants */
#define C_WHITE 7
#define C_GREEN 2
#define C_YELLOW 14
#define C_RED 4
#define C_BLUE 9
#define C_BRED 12

struct tile {
    int angle;      /* 0..345 (mult of 15), or 999 = midspin */
    int twirl;      /* flip sweep direction after this tile */
    int set_bpm;    /* 0=none, else absolute bpm from this tile onward */
    int set_speed;  /* 0=none, else multiplier pct (100=1x) */
};

/* ---- file-level static buffers (kernel stack is only 8KiB) ---- */
static char catbuf[8192];
static struct tile tiles[MAX_TILES];
static int  hit[MAX_TILES];    /* absolute hit time ms */
static int  wbpm[MAX_TILES];   /* effective bpm at tile */
static int  wP[MAX_TILES];     /* Perfect window ms (+-30deg) */
static int  wEP[MAX_TILES];    /* E/LPerfect window ms (+-45deg) */
static int  wE[MAX_TILES];    /* Early/Late window ms (+-60deg) */
static int  tdir[MAX_TILES];   /* sweep direction at tile (+1/-1) */
static int  nb;                /* tile count */
static char title[48];
static int  init_bpm;

/* play state */
static int  now, last_draw, cur;
static int  score, pcount, judged, combo, mp;
static int  won, failed;
static const char *jtext;
static int  jcol;

/* line builder */
static char rb[96];
static int  rl;

static u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(port)); return v; }
static void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "d"(port)); }

static void w(const char *s) { api->write(s); }
static void pc(char c) { api->putc(c); }
static void pads(int n) { int i; for (i = 0; i < n; i++) pc(' '); }

static const char *i2a(int v) {
    static char b[12];
    int i = 11; int neg = 0;
    b[11] = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { b[--i] = '0'; }
    while (v > 0) { b[--i] = '0' + (v % 10); v /= 10; }
    if (neg) b[--i] = '-';
    return b + i;
}

static int smatch(const char *a, const char *b) {
    while (*b) { if (*a != *b) return 0; a++; b++; }
    return 1;
}
static char *findstr(const char *buf, const char *key) {
    const char *p = buf;
    while (*p) { if (smatch(p, key)) return (char *)p; p++; }
    return 0;
}

/* ---- schedule hit times + windows (all integer, remainder carry) ---- */
static void schedule(void) {
    int curbpm = init_bpm;
    int rem = 0;
    int dir = 1;
    int i;
    hit[0] = LEADIN_MS;
    for (i = 0; i < nb; i++) {
        int bpm = curbpm;
        if (bpm < 1) bpm = 1;
        tdir[i] = dir;
        wbpm[i] = bpm;
        {
            int p = 10000 / bpm;   if (p < 25)  p = 25;   wP[i] = p;
            int ep = 15000 / bpm;  if (ep < 30) ep = 30;  wEP[i] = ep;
            int e = 20000 / bpm;   if (e < 65)  e = 65;   wE[i] = e;
        }
        if (i + 1 < nb) {
            int beat = 60000 / bpm;
            rem += 60000 % bpm;
            if (rem >= bpm) { beat += rem / bpm; rem %= bpm; }
            hit[i + 1] = hit[i] + beat;
        }
        if (tiles[i].set_bpm) curbpm = tiles[i].set_bpm;
        if (tiles[i].set_speed) curbpm = curbpm * tiles[i].set_speed / 100;
        if (tiles[i].twirl) dir = -dir;
    }
}

/* ---- .tadofai parser (line based) ---- */
static int parse_tado(const char *buf) {
    const char *p = buf;
    int i;
    init_bpm = 120; nb = 0; title[0] = 0;
    while (*p) {
        char line[128]; int li = 0;
        while (*p && *p != '\n' && li < 127) line[li++] = *p++;
        line[li] = 0;
        if (*p == '\n') p++;
        {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == 0 || *s == '#') continue;
            if (smatch(s, "title:")) {
                char *q = (char *)s + 6; int ti = 0;
                while (*q == ' ') q++;
                while (*q && *q != '\r' && ti < 46) title[ti++] = *q++;
                title[ti] = 0;
                continue;
            }
            if (smatch(s, "bpm:")) {
                int v = 0; char *q = (char *)s + 4;
                while (*q == ' ') q++;
                while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                if (v > 0) init_bpm = v;
                continue;
            }
            if (smatch(s, "tile")) {
                char *q = (char *)s + 4;
                int ang = 0;
                while (*q == ' ') q++;
                while (*q >= '0' && *q <= '9') ang = ang * 10 + (*q++ - '0');
                if (ang != 999 && (ang > 345 || ang % 15 != 0)) return -1;
                if (nb >= MAX_TILES) return -1;
                tiles[nb].angle = ang;
                tiles[nb].twirl = 0; tiles[nb].set_bpm = 0; tiles[nb].set_speed = 0;
                while (*q == ' ') q++;
                while (*q) {
                    while (*q == ' ') q++;
                    if (smatch(q, "twirl")) { tiles[nb].twirl = 1; q += 5; }
                    else if (smatch(q, "speed")) {
                        int v = 0; q += 5;
                        while (*q == ' ') q++;
                        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                        tiles[nb].set_speed = v;
                    }
                    else if (smatch(q, "bpm")) {
                        int v = 0; q += 3;
                        while (*q == ' ') q++;
                        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                        tiles[nb].set_bpm = v;
                    }
                    else break;
                }
                nb++;
                continue;
            }
            /* unknown line: ignore */
        }
    }
    for (i = 0; i < nb; i++) (void)0;
    if (nb == 0) return -1;
    return 0;
}

/* ---- .adofai mini subset parser (byte scan, <=3KB) ---- */
static int read_int_skip(char **pp) {
    char *q = *pp;
    int sign = 1, v = 0, any = 0;
    while (*q && !(*q >= '0' && *q <= '9') && *q != '-') q++;
    if (*q == '-') { sign = -1; q++; }
    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
    *pp = q;
    return any ? sign * v : 0;
}

static int parse_adofai(const char *buf) {
    char *p;
    init_bpm = 120; nb = 0; title[0] = 0;
    p = findstr(buf, "angleData");
    if (!p) {
        if (findstr(buf, "pathData")) return -2;
        return -1;
    }
    p = findstr(p, "[");
    if (!p) return -1;
    p++;
    while (*p && nb < MAX_TILES) {
        while (*p && *p != ']' && !(*p >= '0' && *p <= '9') && *p != '-') p++;
        if (*p == ']' || !*p) break;
        {
            int neg = 0, v = 0;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (neg) v = -v;
            tiles[nb].angle = v;
            tiles[nb].twirl = 0; tiles[nb].set_bpm = 0; tiles[nb].set_speed = 0;
            nb++;
        }
    }
    {
        char *b = findstr(buf, "beatsPerMinute");
        if (b) { char *q = b; while (*q && *q != ':') q++; if (*q == ':') q++; init_bpm = read_int_skip(&q); }
    }
    {
        char *e = (char *)buf;
        while ((e = findstr(e, "\"eventType\"")) != 0) {
            char *set = findstr(e, "SetSpeed");
            char *tw = findstr(e, "Twirl");
            int isSet = (set && (set - e) < 300);
            int isTw = (tw && (tw - e) < 300 && !isSet);
            char *fl = findstr(e, "\"floor\"");
            int floor = -1;
            if (fl) { char *q = fl; while (*q && *q != ':') q++; if (*q == ':') q++; floor = read_int_skip(&q); }
            if (isSet && floor >= 0 && floor < MAX_TILES) {
                char *mul = findstr(e, "Multiplier");
                if (mul && (mul - e) < 300) {
                    char *bb = findstr(e, "bpmMultiplier");
                    if (bb) { char *q = bb; while (*q && *q != ':') q++; if (*q == ':') q++; tiles[floor].set_speed = read_int_skip(&q); }
                } else {
                    char *bb = findstr(e, "beatsPerMinute");
                    if (bb) { char *q = bb; while (*q && *q != ':') q++; if (*q == ':') q++; tiles[floor].set_bpm = read_int_skip(&q); }
                }
            } else if (isTw && floor >= 0 && floor < MAX_TILES) {
                tiles[floor].twirl = 1;
            }
            e++;
        }
    }
    if (nb == 0) return -1;
    return 0;
}

/* ---- frame rendering ---- */
static void rb_reset(void) { rl = 0; rb[0] = 0; }
static void rb_s(const char *s) { while (*s) { rb[rl] = *s++; rb[rl + 1] = 0; rl++; } }
static void rb_i(int v) { rb_s(i2a(v)); }
static void rb_pad(void) { while (rl < 80) { rb[rl] = ' '; rb[rl + 1] = 0; rl++; } }

static void draw(void) {
    int pct, acc, beat, off, col, jl, lead;
    api->cls();
    /* row 0: title + bpm */
    api->set_fg(C_WHITE);
    rb_reset(); rb_s(title[0] ? title : "Cat Beats"); rb_pad(); w(rb); w("\n");
    /* row 1: HUD */
    pct = (nb > 0) ? cur * 100 / nb : 0;
    acc = (judged > 0) ? pcount * 100 / judged : 0;
    rb_reset();
    rb_s("PROGRESS "); rb_i(pct); rb_s("%  SCORE "); rb_i(score);
    rb_s("  ACC "); rb_i(acc); rb_s("%  COMBO "); rb_i(combo);
    rb_pad(); w(rb); w("\n");
    /* rows 2..7 empty */
    w("\n\n\n\n\n\n");
    /* row 8: accuracy bar 10..69 (60 wide) */
    pads(10);
    api->set_fg(C_RED);    { int i; for (i = 0; i < 15; i++) pc('='); }
    api->set_fg(C_YELLOW); { int i; for (i = 0; i < 5; i++) pc('='); }
    api->set_fg(C_GREEN);  { int i; for (i = 0; i < 20; i++) pc('='); }
    api->set_fg(C_YELLOW); { int i; for (i = 0; i < 5; i++) pc('='); }
    api->set_fg(C_RED);    { int i; for (i = 0; i < 15; i++) pc('='); }
    api->set_fg(C_WHITE); pads(10); w("\n");
    /* row 9: sweep marker O, center col 40 */
    beat = 60000 / (wbpm[cur] > 0 ? wbpm[cur] : 100);
    off = (now - hit[cur]) * 60 / beat;
    col = 40 + tdir[cur] * off;
    if (col < 10) col = 10;
    if (col > 69) col = 69;
    api->set_fg(C_WHITE); pads(col); pc('O'); pads(79 - col); w("\n");
    /* row 10: judgment word centered */
    api->set_fg(jcol);
    jl = 0; while (jtext[jl]) jl++;
    lead = 40 - jl / 2; if (lead < 0) lead = 0;
    pads(lead); w(jtext); pads(79 - lead - jl); w("\n");
    /* row 11 empty */
    w("\n");
    /* row 12: hint */
    api->set_fg(C_WHITE);
    rb_reset(); rb_s("  ANY KEY = TAP     ESC = QUIT  "); rb_pad(); w(rb); w("\n");
    /* rows 13..24 empty */
    w("\n\n\n\n\n\n\n\n\n\n\n");
}

static void drain_kb(void) {
    int i;
    for (i = 0; i < 8 && (inb(KB_STATUS) & 1); i++) inb(KB_DATA);
}

/* block until a PS/2 make-code arrives (IRQ1 must be masked by caller).
 * 0xE0 prefix consumes next byte; break codes ignored. Returns make scancode. */
static int wait_key(void) {
    int e0 = 0;
    for (;;) {
        api->delay(1);
        while (inb(KB_STATUS) & 1) {
            u8 sc = inb(KB_DATA);
            if (e0) { e0 = 0; if (!(sc & 0x80)) return sc; }
            else if (sc == 0xE0) e0 = 1;
            else if (!(sc & 0x80)) return sc;
        }
    }
}

/* ---- result screen; returns 1 if ESC pressed (exit CE), 0 otherwise ---- */
static int show_result(void) {
    int pct = (nb > 0) ? cur * 100 / nb : 0;
    int pure = (won && pcount == judged && mp == 0);
    api->cls();
    w("\n\n\n");
    if (won) {
        api->set_fg(C_YELLOW);
        w("              Congratulations!\n\n");
        if (pure) { api->set_fg(C_GREEN); w("               Pure Perfect!\n\n"); }
    } else {
        api->set_fg(C_RED);
        w("                Game over!\n\n");
        w("               PROGRESS "); w(i2a(pct)); w("%\n\n");
    }
    api->set_fg(C_WHITE);
    w("          Restart!  press any key\n");
    w("              (ESC = exit game)\n");
    {
        int c = wait_key();
        return (c == 0x01) ? 1 : 0;   /* scancode 0x01 = ESC */
    }
}

/* ---- play one loaded level; returns 1 if ESC at result -> exit CE ---- */
static int play_level(void) {
    schedule();
    now = 0; last_draw = -100; cur = 0;
    score = 0; pcount = 0; judged = 0; combo = 0; mp = 0;
    won = 0; failed = 0;
    jtext = ""; jcol = C_WHITE;
    api->cls();
    /* IRQ1 already masked by ce_main; poll PS/2 directly */
    while (1) {
        api->delay(1); now++;
        /* drain PS/2, count make-codes; 0x01 = ESC = mid-game quit */
        {
            int presses = 0, e0 = 0, esc = 0;
            while (inb(KB_STATUS) & 1) {
                u8 sc = inb(KB_DATA);
                if (e0) { e0 = 0; if (!(sc & 0x80)) { presses++; if (sc == 0x01) esc = 1; } }
                else if (sc == 0xE0) e0 = 1;
                else if (!(sc & 0x80)) { presses++; if (sc == 0x01) esc = 1; }
            }
            if (esc) return 1;   /* mid-game ESC -> exit CE (caller restores IRQ1) */
            /* auto-pass midspin (999): occupies a beat, no key */
            if (cur < nb && tiles[cur].angle == 999 && now >= hit[cur]) { cur++; }
            if (presses > 0 && cur < nb) {
                int h = hit[cur], e = wE[cur], ep = wEP[cur], p = wP[cur];
                int dt = now - h;
                int adt = dt < 0 ? -dt : dt;
                if (dt < -e) {
                    jtext = "Multipress!!"; jcol = C_WHITE; mp++;
                } else if (adt <= p) {
                    jtext = "Perfect!"; jcol = C_WHITE;
                    score += 2; pcount++; judged++; combo++; cur++;
                } else if (adt <= ep) {
                    jtext = (dt < 0) ? "EPerfect!" : "LPerfect!";
                    jcol = (dt < 0) ? C_BLUE : C_BRED;
                    score += 1; judged++; combo++; cur++;
                } else if (adt <= e) {
                    jtext = (dt < 0) ? "Early!" : "Late!";
                    jcol = C_YELLOW;
                    score += 1; judged++; combo++; cur++;
                } else {
                    jtext = (dt < 0) ? "Early!!" : "Late!!";
                    jcol = C_RED; failed = 1;
                }
            }
        }
        if (failed) break;
        if (cur >= nb) { won = 1; break; }
        /* miss: window closed without a press */
        if (now - hit[cur] > wE[cur]) { jtext = "Miss..."; jcol = C_RED; failed = 1; break; }
        if (now - last_draw >= 33) { draw(); last_draw = now; }
    }
    /* keep IRQ1 masked: result screen also polls PS/2 */
    return show_result();
}

static int load_and_play(const char *path) {
    int n;
    api->cls();
    w("Loading "); w(path); w(" ...\n");
    n = api->cat(path, catbuf, 8191);
    if (n <= 0) {
        api->set_fg(C_RED);
        w("FILE NOT FOUND: "); w(path); w("\n\n");
        api->set_fg(C_WHITE); w("press any key...\n");
        wait_key();
        return 0;
    }
    {
        int r;
        if (findstr(catbuf, "angleData")) r = parse_adofai(catbuf);
        else r = parse_tado(catbuf);
        if (r == -2) {
            api->set_fg(C_RED); w("UNSUPPORTED: pathData\n\n");
            api->set_fg(C_WHITE); w("press any key...\n"); wait_key(); return 0;
        }
        if (r != 0) {
            api->set_fg(C_RED); w("LEVEL PARSE ERROR\n\n");
            api->set_fg(C_WHITE); w("press any key...\n"); wait_key(); return 0;
        }
    }
    return play_level();
}

__attribute__((section(".text.start")))
int ce_main(void) {
    int c;
    outb(PIC_IMR, inb(PIC_IMR) | 0x02);   /* mask IRQ1 for whole CE run */
    for (;;) {
        api->cls();
        w("===== ADOFAI (TEXT) =====\n\n");
        w("  [1] DEMO 1 TUTORIAL\n");
        w("  [2] DEMO 2 TWIRL & SPEED\n");
        w("  [3] LOAD LEVEL\n\n");
        w("  ESC = EXIT\n\n");
        c = wait_key();                    /* scancodes: 0x01 ESC, 0x02..0x04 */
        if (c == 0x01) break;              /* ESC -> exit CE */
        if (c == 0x02) { if (load_and_play("/BIN/DEMO1.TAD")) break; }
        else if (c == 0x03) { if (load_and_play("/BIN/DEMO2.TAD")) break; }
        else if (c == 0x04) {
            char path[64];
            api->cls();
            outb(PIC_IMR, inb(PIC_IMR) & ~0x02);  /* unmask for readline */
            w("Enter level path (.TAD / .AD):\n> ");
            path[0] = 0;
            api->readline(path, 63);
            outb(PIC_IMR, inb(PIC_IMR) | 0x02);    /* re-mask */
            drain_kb();
            if (path[0]) { if (load_and_play(path)) break; }
        }
    }
    outb(PIC_IMR, inb(PIC_IMR) & ~0x02);  /* restore IRQ1 */
    drain_kb();
    return 0;   /* MUST return 0; no api->exit() */
}
