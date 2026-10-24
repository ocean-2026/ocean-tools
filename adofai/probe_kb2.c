/* probe_kb2.c - chunked scan of kernel image tail with file-log progress.
 * For each 128KB chunk: save marker to /BIN/PROBE2.LOG, then scan the chunk
 * for the KB_INPUT struct (ph/pt pointers into own buf, count<=32).
 * If a chunk faults, the app dies and the log shows the last safe chunk.
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

#define SCAN_LO 0x1400000u
#define SCAN_HI 0x1493000u
#define CHUNK    0x20000u   /* 128KB */

static u32 hits[16][3];      /* addr, ph, pt */
static int n_hits;

static u32 rd32(u32 a) { return *(volatile u32 *)a; }

static void scan_chunk(u32 lo, u32 hi) {
    u32 a;
    for (a = lo; a + 48 < hi; a += 4) {
        u32 ph = rd32(a);
        u32 pt = rd32(a + 4);
        u32 cnt = rd32(a + 8);
        if (cnt <= 32 && ph >= a + 12 && ph < a + 44 && pt >= a + 12 && pt < a + 44) {
            if (n_hits < 16) { hits[n_hits][0] = a; hits[n_hits][1] = ph; hits[n_hits][2] = pt; n_hits++; }
        }
    }
}

static void phex(u32 v) {
    char h[] = "0123456789abcdef";
    char b[12]; int i = 0;
    b[i++] = '0'; b[i++] = 'x';
    { int s; for (s = 28; s >= 0; s -= 4) b[i++] = h[(v >> s) & 15]; }
    b[i] = 0; api->write(b);
}

__attribute__((section(".text.start")))
int ce_main(void) {
    u32 c;
    char log[512];
    int lp = 0;
    api->cls();
    api->write("== kb_in tail scan probe ==\n");
    for (c = SCAN_LO; c < SCAN_HI; c += CHUNK) {
        char m[24]; int i;
        static const char hx[] = "0123456789abcdef";
        m[0] = 'c'; m[1] = 0;
        for (i = 0; i < 8; i++) m[1 + i] = hx[((c + CHUNK) >> (28 - i * 4)) & 15];
        m[9] = '-'; m[10] = 0;
        if (lp + 12 < 480) { int j; for (j = 0; j < 11 && m[j]; j++) log[lp++] = m[j]; log[lp++] = '\n'; }
        api->save("/BIN/PROBE2.LOG", log, (u32)lp);
        scan_chunk(c, c + CHUNK);
    }
    api->save("/BIN/PROBE2.LOG", log, (u32)lp);
    api->write("scan done, hits="); api->print_int(n_hits); api->write("\n");
    { int i; for (i = 0; i < n_hits; i++) {
        api->write("  @"); phex(hits[i][0]);
        api->write(" ph="); phex(hits[i][1]);
        api->write(" pt="); phex(hits[i][2]);
        api->write("\n");
    }}
    api->write("press 3 keys to verify...\n");
    { int i; for (i = 0; i < 3; i++) {
        int k = api->getch();
        api->write("key="); phex((u32)k); api->write(" cnt=");
        { int j; int any = 0;
          for (j = 0; j < n_hits; j++) {
              u32 cc = rd32(hits[j][0] + 8);
              if (cc == 1) { api->print_int(j); api->write(":1 "); any = 1; }
          }
          if (!any) api->write("-");
          api->write("\n");
        }
    }}
    api->write("done\n");
    return 0;
}
