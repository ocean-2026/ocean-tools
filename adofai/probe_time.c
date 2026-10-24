/* probe_time.c - CatOS CE timing/input probe
 * Tests: 1) RDTSC availability (CR4.TSD?)  2) delay(1000) ticks calibration
 *        3) time_str precision  4) getch blocking semantics
 */
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef signed int     s32;
typedef unsigned long long u64;

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

static u64 g_tpm;  /* ticks per ms (calibrated) */

/* --- software 64-bit division (no libgcc in -nostdlib) --- */
static u64 udivmod(u64 a, u64 b, u64 *rem) {
    u64 q = 0, r = 0;
    int i;
    if (b == 0) { if (rem) *rem = 0; return 0; }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= (1ULL << i); }
    }
    if (rem) *rem = r;
    return q;
}
u64 __attribute__((used)) __udivdi3(u64 a, u64 b) { return udivmod(a, b, 0); }
u64 __attribute__((used)) __umoddi3(u64 a, u64 b) { u64 r; udivmod(a, b, &r); return r; }
u64 __attribute__((used)) __udivmoddi4(u64 a, u64 b, u64 *rem) { return udivmod(a, b, rem); }

static u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void pdec64(u64 v) {
    char b[24]; int i = 24;
    b[--i] = 0;
    do { b[--i] = '0' + (int)(v % 10); v /= 10; } while (v);
    api->write(b + i);
}

__attribute__((section(".text.start")))
int ce_main(void) {
    char ts[32];
    api->cls();
    api->write("== CE time/input probe ==\n");
    { u64 t0 = rdtsc(), t1 = rdtsc();
      api->write("RDTSC: ok delta="); pdec64(t1 - t0); api->write("\n"); }
    { u64 t0 = rdtsc(); api->delay(1000); u64 t1 = rdtsc();
      g_tpm = (t1 - t0) / 1000;
      api->write("delay(1000): ticks="); pdec64(t1 - t0);
      api->write(" tpm="); pdec64(g_tpm); api->write("\n"); }
    { u64 t0 = rdtsc(); api->delay(1000); u64 t1 = rdtsc();
      api->write("delay#2 ticks="); pdec64(t1 - t0);
      api->write(" ms="); pdec64((t1 - t0) / g_tpm); api->write("\n"); }
    api->time_str(ts); api->write("T0="); api->write(ts); api->write("\n");
    api->delay(3050);
    api->time_str(ts); api->write("T1="); api->write(ts); api->write("\n");
    api->write("getch#1: press a key within ~6s...\n");
    { u64 t0 = rdtsc();
      int c = api->getch();
      u64 dt = rdtsc() - t0;
      api->write("  got key="); api->print_hex((u32)c);
      api->write(" wait_ms="); pdec64(dt / g_tpm); api->write("\n"); }
    api->write("getch#2: (no key planned; if it returns, nonblocking)\n");
    { u64 t0 = rdtsc();
      int c = api->getch();
      u64 dt = rdtsc() - t0;
      api->write("  got key="); api->print_hex((u32)c);
      api->write(" wait_ms="); pdec64(dt / g_tpm); api->write("\n"); }
    api->write("probe done\n");
    return 0;
}
