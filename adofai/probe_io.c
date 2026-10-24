/* probe_io.c - test direct PS/2 port polling from a CE app.
 * Masks IRQ1 (PIC IMR bit1), polls 0x64/0x60 for scancodes, then
 * restores IRQ1 and verifies the shell keyboard still works.
 * Any make-code = one press; break codes skipped; 0xE0 prefix consumed.
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

static u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(port)); return v; }
static void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "d"(port)); }

static void phex(u32 v) {
    char h[] = "0123456789abcdef";
    char b[12]; int i = 0;
    b[i++] = '0'; b[i++] = 'x';
    { int s; for (s = 28; s >= 0; s -= 4) b[i++] = h[(v >> s) & 15]; }
    b[i] = 0; api->write(b);
}

__attribute__((section(".text.start")))
int ce_main(void) {
    int presses = 0;
    int e0 = 0;
    api->cls();
    api->write("== PS/2 port poll probe ==\n");
    api->write("masking IRQ1...\n");
    outb(PIC_IMR, inb(PIC_IMR) | 0x02);
    api->write("press 5 keys (any) now:\n");
    while (presses < 5) {
        api->delay(1);
        if (inb(KB_STATUS) & 1) {
            u8 sc = inb(KB_DATA);
            if (e0) {
                api->write("  E0-ext key="); phex((u32)sc); api->write("\n");
                e0 = 0;
                if (!(sc & 0x80)) presses++;
            } else if (sc == 0xE0) {
                e0 = 1;
            } else if (!(sc & 0x80)) {
                api->write("  make="); phex((u32)sc); api->write("\n");
                presses++;
            }
        }
    }
    api->write("unmasking IRQ1...\n");
    outb(PIC_IMR, inb(PIC_IMR) & ~0x02);
    { int i; for (i = 0; i < 8 && (inb(KB_STATUS) & 1); i++) inb(KB_DATA); }
    api->write("probe done\n");
    return 0;
}
