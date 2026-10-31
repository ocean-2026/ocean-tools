/* host_stub_go.c -- host (Linux/gcc) shim to build go.c as a native binary.
 *
 * Build:
 *   gcc -g -O0 -I <extapp_dir> host_stub_go.c go.c -o go_host
 * Run:
 *   ./go_host [script.GO [arg2 ...]]
 *
 * Paths passed to go.ce (e.g. /BIN/TEST.GO) are mapped: leading "/BIN/"
 * is stripped so the host looks up the file in the current working dir.
 * This lets us run ./go_host /BIN/TEST.GO against ./TEST.GO in cwd.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Re-declare what go.c expects. We do NOT include ce_abi.h here to avoid
 * section attributes; instead we mirror the struct layout exactly. */
typedef void (*ce_ls_cb)(int is_dir, const char *name, uint32_t size, void *ud);

typedef struct catos_api {
    void   (*write)(const char *s);
    void   (*putc)(char c);
    void   (*cls)(void);
    int    (*readline)(char *buf, int max);
    void   (*time_str)(char *out);
    int    (*fs_ready)(void);
    int    (*ls)(const char *path, ce_ls_cb cb, void *ud);
    int    (*touch)(const char *path);
    int    (*mkdir)(const char *path);
    int    (*cat)(const char *path, char *buf, int max);
    int    (*save)(const char *path, const char *data, uint32_t size);
    void * (*alloc)(uint32_t size);
    void   (*free)(void *p);
    void   (*exit)(int code);
    void   (*print_int)(int v);
    void   (*print_hex)(uint32_t v);
    void   (*set_fg)(int color);
    void   (*delay)(uint32_t ms);
    int    (*atoi)(const char *s);
    int    (*strlen)(const char *s);
    int    (*strcmp)(const char *a, const char *b);
    void   (*strcpy)(char *d, const char *s);
    int    (*getch)(void);
    int    (*file_exists)(const char *path);
    int    (*fsize)(const char *path);
    int    (*net_init)(void);
    int    (*net_up)(void);
    int    (*http_get)(const char *url, char *buf, int max);
    int    (*net_dns)(const char *host, char *ip_out);
    int    (*net_udp_send)(const char *ip, int port, const char *data, int len);
    int    (*net_udp_recv)(char *ip, int *port, char *buf, int max);
    int    (*net_ping)(const char *ip);
    void   (*net_ip)(char *out);
    int    (*getuid)(void);
    int    (*gettid)(void);
    int    (*kill)(int tid);
    void   (*yield)(void);
    int    (*hw_num)(int what);
    int    (*hw_str)(int what, char *buf, int max);
    int    (*file_read)(const char *path, char *buf, int max, int offset);
    int    (*file_write)(const char *path, const char *data, int size, int mode);
    int    (*file_delete)(const char *path);
    int    (*pipe_create)(void);
    int    (*pipe_write)(int h, const char *data, int len);
    int    (*pipe_read)(int h, char *buf, int max);
    int    (*pipe_close)(int h);
    int    (*spawn)(const char *path, int argc, char **argv);
    int    (*wait)(int tid);
} catos_api_t;

/* go.c defines __ce_api (initialized to 0). We overwrite it in main(). */
extern catos_api_t *__ce_api;

/* entry called by go.c */
extern int ce_main(int argc, char **argv);

/* ---- host impls ---- */
static void h_write(const char *s) { fputs(s, stdout); }
static void h_putc(char c)        { fputc(c, stdout); }
static void h_cls(void)            { fprintf(stdout, "\x1b[2J\x1b[H"); }
static int h_readline(char *buf, int max) {
    if (!fgets(buf, max, stdin)) return 0;
    return (int)strlen(buf);
}
static void h_time_str(char *out) { strcpy(out, "2026-01-01 00:00:00"); }
static int h_fs_ready(void) { return 1; }
static int h_ls(const char *p, ce_ls_cb cb, void *ud) { (void)p;(void)cb;(void)ud; return -1; }
static int h_touch(const char *p) { int fd = creat(p, 0644); if (fd<0) return -1; close(fd); return 0; }
static int h_mkdir(const char *p) { return mkdir(p, 0755); }
static int h_cat(const char *p, char *buf, int max) {
    int fd = open(p, O_RDONLY); if (fd<0) return -1;
    int n = read(fd, buf, max-1); close(fd);
    if (n<0) return -1; buf[n]=0; return n;
}
static int h_save(const char *p, const char *data, uint32_t size) {
    int fd = open(p, O_WRONLY|O_CREAT|O_TRUNC, 0644); if (fd<0) return -1;
    int n = write(fd, data, size); close(fd); return n;
}
static void *h_alloc(uint32_t n) { return malloc(n ? n : 1); }
static void  h_free(void *p)     { free(p); }
static void  h_exit(int c)       { exit(c); }
static void  h_print_int(int v)  { printf("%d", v); }
static void  h_print_hex(uint32_t v) { printf("%x", v); }
static void  h_set_fg(int c)     { (void)c; }
static void  h_delay(uint32_t ms){ usleep(ms*1000); }
static int   h_atoi(const char *s) { return atoi(s); }
static int   h_strlen(const char *s) { return (int)strlen(s); }
static int   h_strcmp(const char *a, const char *b) { return strcmp(a,b); }
static void  h_strcpy(char *d, const char *s) { strcpy(d,s); }
static int   h_getch(void) { return -1; }

/* Map /BIN/FOO.GO -> ./FOO.GO ; otherwise use path as-is. */
static const char *h_map(const char *path, char *tmp, int tlen) {
    if (strncmp(path, "/BIN/", 5) == 0) {
        snprintf(tmp, tlen, "./%s", path+5);
        return tmp;
    }
    if (strcmp(path, "/BIN") == 0 || strcmp(path, "/") == 0) {
        snprintf(tmp, tlen, "."); return tmp;
    }
    return path;
}
static int h_file_exists(const char *path) {
    char tmp[512]; const char *p = h_map(path, tmp, sizeof tmp);
    return access(p, F_OK) == 0 ? 1 : 0;
}
static int h_fsize(const char *path) {
    char tmp[512]; const char *p = h_map(path, tmp, sizeof tmp);
    struct stat st; if (stat(p, &st)!=0) return -1; return (int)st.st_size;
}
static int h_file_read(const char *path, char *buf, int max, int offset) {
    /* honor max<=16384 per call (kernel limit); host itself can read more
     * but we deliberately clamp to exercise the chunking path. */
    if (max > 16384) max = 16384;
    char tmp[512]; const char *p = h_map(path, tmp, sizeof tmp);
    int fd = open(p, O_RDONLY); if (fd<0) return -1;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) { close(fd); return -3; }
    int n = read(fd, buf, max); close(fd);
    return n < 0 ? -1 : n;
}
static int h_file_write(const char *path, const char *data, int size, int mode) {
    char tmp[512]; const char *p = h_map(path, tmp, sizeof tmp);
    int fl = O_WRONLY|O_CREAT|(mode==1 ? O_APPEND : O_TRUNC);
    int fd = open(p, fl, 0644); if (fd<0) return -1;
    int n = write(fd, data, size); close(fd); return n;
}
static int h_file_delete(const char *path) {
    char tmp[512]; const char *p = h_map(path, tmp, sizeof tmp);
    return unlink(p) == 0 ? 0 : -1;
}
static int h_pipe_create(void) { return -1; }
static int h_pipe_write(int h, const char *d, int l) { (void)h;(void)d;(void)l; return -1; }
static int h_pipe_read(int h, char *b, int m) { (void)h;(void)b;(void)m; return -1; }
static int h_pipe_close(int h) { (void)h; return -1; }
static int h_spawn(const char *p, int ac, char **av) { (void)p;(void)ac;(void)av; return -1; }
static int h_wait(int t) { (void)t; return -1; }
static int h_net_init(void) { return -1; }
static int h_net_up(void) { return 0; }
static int h_http_get(const char *u, char *b, int m) { (void)u;(void)b;(void)m; return -1; }
static int h_net_dns(const char *h, char *o) { (void)h;(void)o; return -1; }
static int h_net_udp_send(const char *ip, int p, const char *d, int l) { (void)ip;(void)p;(void)d;(void)l; return -1; }
static int h_net_udp_recv(char *ip, int *p, char *b, int m) { (void)ip;(void)p;(void)b;(void)m; return -1; }
static int h_net_ping(const char *ip) { (void)ip; return -1; }
static void h_net_ip(char *o) { strcpy(o, "0.0.0.0"); }
static int h_getuid(void) { return 0; }
static int h_gettid(void) { return 1; }
static int h_kill(int t) { (void)t; return -1; }
static void h_yield(void) { }
static int h_hw_num(int w) { (void)w; return 0; }
static int h_hw_str(int w, char *b, int m) { (void)w;(void)b;(void)m; return -1; }

static catos_api_t g_api = {
    h_write, h_putc, h_cls, h_readline, h_time_str, h_fs_ready,
    h_ls, h_touch, h_mkdir, h_cat, h_save,
    h_alloc, h_free, h_exit,
    h_print_int, NULL, NULL, h_delay,
    h_atoi, h_strlen, h_strcmp, h_strcpy, h_getch,
    h_file_exists, h_fsize,
    h_net_init, h_net_up, h_http_get,
    h_net_dns, h_net_udp_send, h_net_udp_recv, h_net_ping, h_net_ip,
    h_getuid, h_gettid, h_kill, h_yield,
    h_hw_num, h_hw_str,
    h_file_read, h_file_write, h_file_delete,
    h_pipe_create, h_pipe_write, h_pipe_read, h_pipe_close,
    h_spawn, h_wait,
};

int main(int argc, char **argv) {
    __ce_api = &g_api;
    int rc = ce_main(argc, argv);
    return rc;
}
