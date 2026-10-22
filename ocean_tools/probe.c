/* probe.c - CE API table prober for CatOS
 * Calls each API slot with safe args and prints the slot index + return value.
 * Usage: run probe <start> <end>   (slot indices)
 * Build chain: gcc -m32 freestanding -> ld (ce.ld, base 0x02000000) -> mkce.py
 */
typedef unsigned int u32;

typedef u32 (*fat_fn)(u32, u32, u32, u32, u32);
typedef struct { fat_fn p[33]; } api_t;

static api_t *__ce_api __attribute__((section(".ce_api"))) = (void *)0;

static char buf[80];
static char CONST[16] = "ABC";
static char outbuf[32];

static void w(const char *s) { __ce_api->p[0]( (u32)s, 0, 0, 0, 0 ); }

static void dec(u32 v) {
    char tmp[16]; int n = 0;
    if (v == 0) { w("0"); return; }
    while (v && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) w((char[2]){ tmp[--n], 0 });
}

static void hex(u32 v) {
    char t[12]; int i;
    t[0]='0'; t[1]='x';
    for (i = 9; i >= 2; i--) { t[i] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    t[10] = 0;
    w(t);
}

static u32 parse(const char *s) {
    u32 r = 0;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (u32)(*s - '0'); s++; }
    return r;
}

int ce_main(int argc, char **argv) __attribute__((section(".text.start")));
int ce_main(int argc, char **argv) {
    u32 start = 0, end = 33;
    if (argc >= 2) start = parse(argv[1]);
    if (argc >= 3) end = parse(argv[2]);
    w("PROBE ");
    dec(start); w("-"); dec(end); w("\n");
    for (u32 i = start; i < end; i++) {
        /* print marker "S<i>:" before call (hang detection) */
        w("S"); dec(i); w(":");
        /* safe args: (outbuf, outbuf, 1, 0, 0) */
        for (int k = 0; k < 32; k++) outbuf[k] = 0;
        u32 r = __ce_api->p[i]( (u32)outbuf, (u32)CONST, 10, 0, 0 );
        w("R"); hex(r);
        if (outbuf[0]) { w(" buf="); w(outbuf); }
        w("\n");
    }
    w("PROBE done, calling exit(0)...\n");
    __ce_api->p[13](0,0,0,0,0);
    w("PROBE survived exit - BUG\n");
    return 0;
}
