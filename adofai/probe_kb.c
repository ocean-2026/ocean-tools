/* probe_kb.c - locate the kernel keyboard ring buffer (kb_in) via memory scan,
 * then verify by keypress (count 0->1->0). Proves memory-poll input works.
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

#define KB_BASE  0x1000000u
#define KB_END   0x1400000u
#define MAX_CAND 32

static u32 cand[MAX_CAND];
static u32 cand_ph[MAX_CAND], cand_pt[MAX_CAND], cand_cnt[MAX_CAND];
static int n_cand;

static u32 rd32(u32 a) { return *(volatile u32 *)a; }

static void scan_candidates(void) {
    u32 a;
    n_cand = 0;
    for (a = KB_BASE; a + 48 < KB_END && n_cand < MAX_CAND; a += 4) {
        u32 ph = rd32(a);
        u32 pt = rd32(a + 4);
        u32 cnt = rd32(a + 8);
        if (cnt <= 32 && ph >= a + 12 && ph < a + 44 && pt >= a + 12 && pt < a + 44) {
            cand[n_cand] = a;
            cand_ph[n_cand] = ph;
            cand_pt[n_cand] = pt;
            cand_cnt[n_cand] = cnt;
            n_cand++;
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
    int i;
    api->cls();
    api->write("== kb_in scan probe ==\n");
    scan_candidates();
    api->write("candidates: "); api->print_int(n_cand); api->write("\n");
    for (i = 0; i < n_cand; i++) {
        api->write("  cand "); api->print_int(i); api->write(" @");
        phex(cand[i]);
        api->write(" ph="); phex(cand_ph[i]);
        api->write(" pt="); phex(cand_pt[i]);
        api->write(" cnt="); api->print_int((int)cand_cnt[i]); api->write("\n");
    }
    if (n_cand == 0) { api->write("NO CANDIDATES\n"); return 0; }
    api->write("press 5 keys; each should toggle one cand's cnt 0->1->0\n");
    for (i = 0; i < 5; i++) {
        int k = api->getch();
        api->write("key="); phex((u32)k); api->write(" -> ");
        { int j; int hit = -1;
          for (j = 0; j < n_cand; j++) {
              u32 c = rd32(cand[j] + 8);
              if (c == 1) { hit = j; }
          }
          if (hit >= 0) { api->write("cand "); api->print_int(hit); api->write(" count=1\n"); }
          else api->write("no cand saw count=1\n");
        }
    }
    api->write("probe done\n");
    return 0;
}
