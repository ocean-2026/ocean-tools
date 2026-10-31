/* host_stub.c - run cc.c as a normal host program to test parsing/codegen
 * without QEMU.  The api table is backed by host stdio + files under
 * ROOT (env CCDEV_ROOT, default ./root).  Linked together with cc.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ce_abi.h"

static const char *root(void) {
    const char *r = getenv("CCDEV_ROOT");
    return r ? r : "./root";
}
static void host_path(char *out, const char *p) {
    /* strip leading /, join root */
    while (p && *p == '/') p++;
    snprintf(out, 512, "%s/%s", root(), p);
}

static void h_write(const char *s) { fputs(s, stdout); }
static void h_putc(char c) { fputc(c, stdout); }
static void h_cls(void) { printf("\033[2J\033[H"); }
static int h_readline(char *buf, int max) {
    if (!fgets(buf, max, stdin)) { buf[0]=0; return 0; }
    int n = strlen(buf); if (n && buf[n-1]=='\n') buf[n-1]=0;
    return (int)strlen(buf);
}
static void h_time_str(char *out){ strcpy(out,"2026-10-31 08:00:00"); }
static int h_fs_ready(void){ return 1; }
static int h_ls(const char *p, ce_ls_cb cb, void *ud){ (void)p;(void)cb;(void)ud; return 0; }
static int h_touch(const char *p){ char b[512]; host_path(b,p); FILE*f=fopen(b,"wb"); if(!f)return -1; fclose(f); return 0; }
static int h_mkdir(const char *p){ (void)p; return 0; }
static int h_cat(const char *p, char *buf, int max){
    char b[512]; host_path(b,p); FILE*f=fopen(b,"rb"); if(!f) return -1;
    int n=(int)fread(buf,1,max,f); fclose(f); return n;
}
static int h_save(const char *p, const char *d, uint32_t sz){
    char b[512]; host_path(b,p); FILE*f=fopen(b,"wb"); if(!f)return -1;
    fwrite(d,1,sz,f); fclose(f); return 0;
}
static void *h_alloc(uint32_t sz){ return malloc(sz ? sz : 1); }
static void h_free(void *p){ free(p); }
static void h_exit(int c){ exit(c); }
static void h_print_int(int v){ printf("%d", v); }
static void h_print_hex(uint32_t v){ printf("%x", v); }
static void h_set_fg(int c){ (void)c; }
static void h_delay(uint32_t ms){ (void)ms; }
static int h_atoi(const char *s){ return atoi(s); }
static int h_strlen(const char *s){ return (int)strlen(s); }
static int h_strcmp(const char *a, const char *b){ return strcmp(a,b); }
static void h_strcpy(char *d, const char *s){ strcpy(d,s); }
static int h_getch(void){ return -1; }
static int h_file_exists(const char *p){ char b[512]; host_path(b,p); FILE*f=fopen(b,"rb"); if(!f)return 0; fclose(f); return 1; }
static int h_fsize(const char *p){ char b[512]; host_path(b,p); FILE*f=fopen(b,"rb"); if(!f)return -1; fseek(f,0,SEEK_END); int n=ftell(f); fclose(f); return n; }
static int h_net_init(void){ return -1; }
static int h_net_up(void){ return 0; }
static int h_http_get(const char*u,char*b,int m){(void)u;(void)b;(void)m;return -1;}
static int h_net_dns(const char*h,char*o){(void)h;strcpy(o,"0.0.0.0");return 0;}
static int h_net_udp_send(const char*ip,int p,const char*d,int l){(void)ip;(void)p;(void)d;(void)l;return -1;}
static int h_net_udp_recv(char*ip,int*port,char*b,int m){(void)ip;(void)port;(void)b;(void)m;return -1;}
static int h_net_ping(const char*ip){(void)ip;return -1;}
static void h_net_ip(char*o){strcpy(o,"0.0.0.0");}
static int h_getuid(void){return 0;}
static int h_gettid(void){return 1;}
static int h_kill(int t){(void)t;return -1;}
static void h_yield(void){}
static int h_hw_num(int w){(void)w;return 0;}
static int h_hw_str(int w,char*b,int m){(void)w;snprintf(b,m,"hw");return 0;}

static int h_file_read(const char *p, char *buf, int max, int off){
    char b[512]; host_path(b,p); FILE*f=fopen(b,"rb"); if(!f) return -1;
    fseek(f, off, SEEK_SET);
    int n=(int)fread(buf,1,max,f); fclose(f); return n;
}
static int h_file_write(const char *p, const char *data, int size, int mode){
    char b[512]; host_path(b,p);
    FILE*f=fopen(b, mode==0 ? "wb" : "ab"); if(!f) return -1;
    int n=(int)fwrite(data,1,size,f); fclose(f); return n;
}
static int h_file_delete(const char*p){char b[512];host_path(b,p);remove(b);return 0;}
static int h_pipe_create(void){return -1;}
static int h_pipe_write(int h,const char*d,int l){(void)h;(void)d;(void)l;return -1;}
static int h_pipe_read(int h,char*b,int m){(void)h;(void)b;(void)m;return -1;}
static int h_pipe_close(int h){(void)h;return -1;}
static int h_spawn(const char*p,int a,char**v){(void)p;(void)a;(void)v;return -1;}
static int h_wait(int t){(void)t;return -1;}

catos_api_t __api = {
    h_write, h_putc, h_cls, h_readline, h_time_str, h_fs_ready, h_ls,
    h_touch, h_mkdir, h_cat, h_save, h_alloc, h_free, h_exit,
    h_print_int, h_print_hex, h_set_fg, h_delay, h_atoi, h_strlen,
    h_strcmp, h_strcpy, h_getch, h_file_exists, h_fsize,
    h_net_init, h_net_up, h_http_get, h_net_dns, h_net_udp_send,
    h_net_udp_recv, h_net_ping, h_net_ip, h_getuid, h_gettid, h_kill,
    h_yield, h_hw_num, h_hw_str, h_file_read, h_file_write, h_file_delete,
    h_pipe_create, h_pipe_write, h_pipe_read, h_pipe_close, h_spawn, h_wait
};
/* cc.c defines __ce_api itself; we assign it from main. */
extern int ce_main(int argc, char **argv);
int main(int argc, char **argv) {
    extern catos_api_t *__ce_api;
    __ce_api = &__api;
    /* default invocation if none given */
    if (argc < 3) {
        char *av[] = {"cc","/BIN/HELLO.C","/BIN/OUT.CE",0};
        return ce_main(3, av);
    }
    return ce_main(argc, argv);
}
