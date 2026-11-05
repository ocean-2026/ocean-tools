#include "shell.h"
#include "vga.h"
#include "gfx.h"
#include "keyboard.h"
#include "rtc.h"
#include "io.h"
#include "fs.h"
#include "ata.h"
#include "fat.h"
#include "ce.h"
#include "net.h"
#include "task.h"
#include "hw.h"

void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid);

/* ---------- tiny libc ---------- */
static int strcmp_(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void itoa(int v, char *out, int width) {
    char tmp[12]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i < width) tmp[i++] = '0';
    if (neg) tmp[i++] = '-';
    int j = 0; while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void print_int(int v) {
    char b[12]; itoa(v, b, 0); vga_write(b);
}
static void print_hex(uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { vga_write("0"); return; }
    while (v > 0) { b[i++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    while (i > 0) { char c = b[--i]; vga_putc(c); }
}
static void print_pad(int v, int w) {
    char b[12]; itoa(v, b, w); vga_write(b);
}

/* ---------- multiboot info ---------- */
extern uint32_t multiboot_info_ptr;

typedef struct {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} multiboot_info_t;

static multiboot_info_t *mbi;

/* ---------- graphics mode demo ---------- */
static void cmd_gfx(void) {
    vga_gfx_enable();
    /* gradient bands */
    for (int i = 0; i < 8; i++)
        gfx_fillrect(0, i * 25, GFX_W, 25, (uint8_t)(i * 2 + 1));
    /* frame + grid */
    gfx_rect(0, 0, GFX_W, GFX_H, 15);
    for (int x = 40; x < GFX_W; x += 40) gfx_vline(x, 1, GFX_H - 2, 8);
    for (int y = 25; y < GFX_H; y += 25) gfx_hline(1, y, GFX_W - 2, 8);
    /* title banner */
    gfx_fillrect(56, 40, 208, 32, 1);
    gfx_rect(56, 40, 208, 32, 11);
    gfx_text(64, 48, "CatOS Graphics Mode", 15, 1);
    /* colour swatches */
    for (int i = 0; i < 16; i++)
        gfx_fillrect(16 + i * 18, 88, 16, 16, (uint8_t)i);
    /* diagonal lines */
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 8; j++)
            gfx_putpixel(i * 32 + j, 104 + i * 8 + j, (uint8_t)(i + 4));
    /* info text */
    gfx_text(24, 130, "320x200x256  Mode 13h  DAC palette", 11, 0);
    gfx_text(24, 142, "8x8 bitmap font  soft terminal 40x25", 10, 0);
    gfx_text(24, 154, "type 'text' to switch back", 14, 0);
    gfx_text(24, 166, "CatOS v0.3 five-pillar kernel", 9, 0);
}

static void cmd_text(void) {
    vga_gfx_disable();
}

/* ---------- commands ---------- */
static void cmd_help(void) {
    vga_write(
        "CatOS v0.3 (five-pillar kernel) - commands:\n"
        "  help         show this message\n"
        "  clear        clear the screen\n"
        "  echo TEXT    print text back\n"
        "  about        system info banner\n"
        "  uname        print OS name\n"
        "  date         show RTC date/time\n"
        "  mem          show RAM size (multiboot)\n"
        "  color NUM    set foreground color (0-15)\n"
        "  reboot       warm reboot the machine\n"
        "  hang         halt the CPU (QEMU: close window)\n"
        "  -- processes (ring3 tasks, preemptive) --\n"
        "  run FILE.ce [args..] [&]  execute a CE app (in background with &)\n"
        "  ps           list tasks (tid, uid, ring, state)\n"
        "  kill TID     terminate a task\n"
        "  sudo CMD     run a program as root (uid 0)\n"
        "  uptime       seconds since boot + task count\n"
        "  top          live task monitor (CPU% per task, any key quits)\n"
        "  hwinfo       full hardware dump (CPU/mem/disk/NIC/PCI)\n"
        "  -- network (PCnet) --\n"
        "  curl URL     HTTP GET (http://IP[:port]/path)\n"
        "  ping IP      ICMP echo (3 tries)\n"
        "  dns NAME     resolve a hostname\n"
        "  udp IP PORT TEXT  send one UDP datagram and wait for a reply\n"
        "  netdiag      dump NIC/driver state\n"
        "  Program lookup: builtin -> current dir .ce -> /bin .ce (PATH)\n"
        "  -- filesystem (FAT32 on IDE, persistent) --\n"
        "  dir [PATH]   list a directory\n"
        "  cd [PATH]    change directory (use .. or /)\n"
        "  mkdir PATH   create a directory\n"
        "  touch FILE   create an empty file\n"
        "  format       erase the FAT32 volume (y/n confirm)\n"
        "  edit FILE    nano-style full-screen editor\n"
        "               (arrows move, ^O save, ^X exit, ^K del line, ^U clear)\n"
    );
}

static void cmd_about(void) {
    vga_set_fg(VGA_LIGHT_CYAN);
    vga_write(
        "   _____      _        ____   _____ \n"
        "  / ____|    | |      / __ \\ / ____|\n"
        " | |     __ _| |_  ___| |  | | (___  \n"
        " | |    / _` | __|/ _ \\ |  | |\\___ \\ \n"
        " | |___| (_| | |_  __/ |__| |____) |\n"
        "  \\\_____\\__,_|\\__|\\___|\\____/|_____/ \n"
    );
    vga_set_fg(VGA_LIGHT_GREY);
    vga_write("  A tiny Unix-like OS on GRUB Multiboot\n");
    vga_write("  Architecture : i386 (32-bit x86)\n");
    vga_write("  Bootloader   : GRUB 2 (ISO9660 / El Torito)\n");
    vga_write("  Kernel       : interrupts + paging + tasks\n");
    vga_write("                 ring3 CE apps via int 0x80, uid perms\n");
    vga_write("  Display      : VGA text mode 80x25\n");
    vga_write("  Input        : PS/2 keyboard (IRQ-driven)\n");
    vga_write("  Filesystem   : FAT32 on IDE (persistent)\n");
    vga_write("  Runs on      : QEMU / VMware / Bochs / real PC\n");
}

static void cmd_uname(void) {
    vga_write("CatOS i386 0.3 (grub-multiboot, 5-pillar kernel)\n");
}

static void cmd_date(void) {
    rtc_time_t t; rtc_read(&t);
    vga_write("Current RTC: ");
    print_pad(t.year, 4); vga_write("-");
    print_pad(t.month, 2); vga_write("-");
    print_pad(t.day, 2); vga_write(" ");
    print_pad(t.hour, 2); vga_write(":");
    print_pad(t.minute, 2); vga_write(":");
    print_pad(t.second, 2);
    vga_write("\n");
}

static void cmd_mem(void) {
    if (mbi && (mbi->flags & 1)) {
        vga_write("Lower memory (0-640K): "); print_int(mbi->mem_lower); vga_write(" KB\n");
        vga_write("Upper memory (>1MB)  : "); print_int(mbi->mem_upper); vga_write(" KB\n");
        vga_write("Total approx         : ");
        print_int((mbi->mem_lower + mbi->mem_upper) / 1024);
        vga_write(" MB\n");
    } else {
        vga_write("Memory info not provided by GRUB.\n");
    }
}

static void cmd_reboot(void) {
    vga_write("Rebooting...\n");
    outb(0x64, 0xFE);
    while (1) __asm__ volatile ("hlt");
}

static void cmd_hang(void) {
    vga_write("Halting. Press Ctrl-Alt-Del or close the VM.\n");
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

/* ---------- filesystem commands ---------- */
static void cmd_cd(const char *arg) {
    int r = fs_chdir(arg);
    if (r == FS_ERR_NOENT)      vga_write("cd: no such directory\n");
    else if (r == FS_ERR_NOTDIR) vga_write("cd: not a directory\n");
}

static void cmd_mkdir(const char *arg) {
    if (!*arg) { vga_write("usage: mkdir PATH\n"); return; }
    int r = fs_mkdir(arg);
    if (r == FS_OK)            return;
    if (r == FS_ERR_NOENT)      vga_write("mkdir: parent directory not found\n");
    else if (r == FS_ERR_NOTDIR) vga_write("mkdir: parent is not a directory\n");
    else if (r == FS_ERR_EXIST) vga_write("mkdir: file or directory already exists\n");
    else if (r == FS_ERR_INVAL) vga_write("mkdir: invalid name\n");
    else if (r == FS_ERR_FULL)  vga_write("mkdir: disk full (FAT32)\n");
}

static void cmd_touch(const char *arg) {
    if (!*arg) { vga_write("usage: touch FILE\n"); return; }
    int r = fs_touch(arg);
    if (r == FS_OK)            return;
    if (r == FS_ERR_NOENT)      vga_write("touch: parent directory not found\n");
    else if (r == FS_ERR_NOTDIR) vga_write("touch: is a directory\n");
    else if (r == FS_ERR_INVAL) vga_write("touch: invalid name\n");
    else if (r == FS_ERR_FULL)  vga_write("touch: disk full (FAT32)\n");
}

/* split a string into words (in place); returns word count, max bounded */
static int split_words(char *s, char **out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        out[n++] = s;
        while (*s && *s != ' ') s++;
        if (*s) { *s = 0; s++; }
    }
    return n;
}

/* bounded concatenation: never writes past d+size; returns 0 if it fit */
static int strcat_b(char *d, int size, const char *s) {
    while (*d && size > 0) { d++; size--; }
    if (size <= 1) return -1;
    while (*s && size > 1) { *d++ = *s++; size--; }
    *d = 0;
    return *s ? -1 : 0;
}

/* run "cmd" as a CE program: current directory first, then /bin (PATH).
 * Returns 0 when a program was found (spawn attempted), -1 if not found. */
static int run_from_path(const char *cmd, char **argv, int argc, int bg, int uid) {
    if (!cmd || !*cmd) return -1;
    if (!fs_fs_ready()) return -1;
    char p[FS_PATH_MAX];
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    uint32_t c = 0, s = 0;

    /* 1) current directory (bounded: never overflow like the old code) */
    p[0] = 0;
    if (!(pwd[0] == '/' && pwd[1] == 0)) {
        if (strcat_b(p, sizeof(p), pwd) != 0) return -1;
        if (strcat_b(p, sizeof(p), "/") != 0) return -1;
    } else {
        strcat_b(p, sizeof(p), "/");
    }
    if (strcat_b(p, sizeof(p), cmd) != 0) return -1;
    if (strcat_b(p, sizeof(p), ".ce") != 0) return -1;
    if (fs_resolve_file(p, &c, &s) == 0) {
        spawn_and_maybe_wait(p, argc, argv, bg, uid);
        return 0;
    }
    /* 2) PATH: /bin */
    p[0] = 0;
    if (strcat_b(p, sizeof(p), "/bin/") != 0) return -1;
    if (strcat_b(p, sizeof(p), cmd) != 0) return -1;
    if (strcat_b(p, sizeof(p), ".ce") != 0) return -1;
    if (fs_resolve_file(p, &c, &s) == 0) {
        spawn_and_maybe_wait(p, argc, argv, bg, uid);
        return 0;
    }
    return -1;
}

/* spawn a .ce as a ring3 task; foreground runs wait for it to exit,
 * background ('&') returns to the shell immediately */
void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write("run: cannot load '");
        vga_write(path);
        vga_write("' (");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    if (bg) {
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    task_wait_child(task_by_tid(tid));
}

/* ---- v2.5: shell redirection (| / > / >>) ----------------------------- */

/* split "cmd args [> F | >> F | | CMD]" into out_redir / pipe_to and
 * truncate arg at the operator.  Returns 1 when a redirection exists. */
static int parse_redir(char *arg, char **out_redir, int *append, char **pipe_to) {
    *out_redir = 0; *append = 0; *pipe_to = 0;
    for (char *q = arg; *q; q++) {
        if (*q == '|') { *pipe_to = q + 1; *q = 0; break; }
        if (*q == '>') {
            if (*(q + 1) == '>') { *append = 1; *out_redir = q + 2; *(q + 1) = 0; }
            else                 { *out_redir = q + 1; }
            *q = 0;
            break;
        }
    }
    if (*out_redir) while (**out_redir == ' ') (*out_redir)++;
    if (*pipe_to)   while (**pipe_to == ' ')   (*pipe_to)++;
    return (*out_redir || *pipe_to) ? 1 : 0;
}

/* make a redirection target absolute against the current directory */
static int redir_abspath(const char *name, char *out, int outlen) {
    if (!name || !*name) return -1;
    out[0] = 0;
    if (name[0] == '/') return strcat_b(out, outlen, name) == 0 ? 0 : -1;
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    if (pwd[0] == '/' && pwd[1] == 0) {
        if (strcat_b(out, outlen, "/") != 0) return -1;
    } else {
        if (strcat_b(out, outlen, pwd) != 0) return -1;
        if (strcat_b(out, outlen, "/") != 0) return -1;
    }
    return strcat_b(out, outlen, name) == 0 ? 0 : -1;
}

/* spawn a .ce and wire optional redirection: out_file (append flag) and/or
 * pipe ends.  fg = 1 waits for exit (and the shell then reaps the task). */
static void spawn_ce_redirect(const char *path, int argc, char **argv,
                              int fg, int uid,
                              const char *out_file, int append,
                              int out_pipe, int in_pipe) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write("run: cannot load '");
        vga_write(path);
        vga_write("' (err ");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    task_t *tt = task_by_tid(tid);
    if (out_file && out_file[0]) {
        char ab[FS_PATH_MAX];
        if (redir_abspath(out_file, ab, sizeof(ab)) != 0) {
            vga_write("run: bad redirect path\n");
            task_kill(tid, UID_ROOT);
            return;
        }
        int i;
        for (i = 0; ab[i] && i < (int)sizeof(tt->out_file) - 1; i++)
            tt->out_file[i] = ab[i];
        tt->out_file[i] = 0;
        /* v2.5.1 (FAIL-5-1 fix): `>` truncates once up front, then every
         * SYS_WRITE must append so multi-line output is not clobbered. */
        tt->out_append = 1;
        if (!append)
            fs_write_raw_mode(ab, "", 0, 0);      /* pre-create/truncate; #49: 无条件截断 */
    }
    if (out_pipe >= 0) kpipe_assign_out(tid, out_pipe);
    if (in_pipe  >= 0) kpipe_assign_in(tid, in_pipe);
    if (!fg) {
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    task_wait_child(tt);
}

static void vga_clear_disabled(void) { }
static void cmd_run(char *arg, int uid) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: run FILE.ce [args...] [&] [> F] [>> F] [| CMD]\n");
        return;
    }
    if (!fs_fs_ready()) { vga_write("run: no FAT32 volume.\n"); return; }

    char *out_redir = 0, *pipe_to = 0; int append = 0;
    parse_redir(arg, &out_redir, &append, &pipe_to);

    /* split FILE.ce from its arguments */
    char *file = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; }
    char *argv[16];
    int argc = 1;
    argv[0] = file;
    argc += split_words(arg, argv + 1, 15);
    int bg = 0;
    if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
        bg = 1;
        argc--;
    }

    if (pipe_to && *pipe_to) {
        /* a | b : producer (background, writes the pipe) then consumer
         * (foreground, reads the pipe until EOF when the writer exits) */
        char *file2 = pipe_to;
        char *sp = pipe_to;
        while (*sp && *sp != ' ') sp++;
        if (*sp) { *sp = 0; sp++; }
        char *argv2[16];
        int argc2 = 1;
        argv2[0] = file2;
        argc2 += split_words(sp, argv2 + 1, 15);
        int h = kpipe_create();
        if (h < 0) { vga_write("run: no pipe slots\n"); return; }
        vga_clear();
        spawn_ce_redirect(file, argc, argv, 0, uid, 0, 0, h, -1);
        spawn_ce_redirect(file2, argc2, argv2, 1, uid, 0, 0, -1, h);
        return;
    }
    vga_clear_disabled();
    spawn_ce_redirect(file, argc, argv, !bg, uid, out_redir, append, -1, -1);
}

/* ---------- netcfg: view/change IP config (VMware NAT etc.) ---------- */
static int ip4_parse_(const char *s, uint8_t out[4]) {
    int v = 0, n = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (const char *q = s; *q; q++) {
        if (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); if (v > 255) return 0; }
        else if (*q == '.') { if (n >= 3) return 0; out[n++] = (uint8_t)v; v = 0; }
        else return 0;
    }
    out[n++] = (uint8_t)v;
    return n == 4;
}

static void cmd_netcfg(char *arg) {
    extern uint8_t net_our_ip[4];
    extern uint8_t net_gw_ip[4];
    extern void net_dns_server_set(const uint8_t ip[4]);
    extern const uint8_t *net_dns_server_get(void);
    extern void net_arp_flush(void);

    if (!*arg) {
        vga_write("ip  : ");
        for (int i = 0; i < 4; i++) { print_int(net_our_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  gw: ");
        for (int i = 0; i < 4; i++) { print_int(net_gw_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  dns: ");
        const uint8_t *d = net_dns_server_get();
        for (int i = 0; i < 4; i++) { print_int(d[i]); if (i < 3) vga_write("."); }
        vga_write("\nusage: netcfg IP GATEWAY [DNS]");
        vga_write("   e.g. netcfg 192.168.152.8 192.168.152.1 192.168.152.2\n");
        return;
    }
    uint8_t ip[4], gw[4], dns[4];
    char *w[4];
    int n = 0;
    w[n++] = arg;
    for (char *q = arg; *q && n < 4; q++)
        if (*q == ' ') { *q = 0; while (q[1] == ' ') q++; w[n++] = q + 1; }
    if (n < 2 || !ip4_parse_(w[0], ip) || !ip4_parse_(w[1], gw)) {
        vga_write("netcfg: bad arguments\n");
        return;
    }
    for (int i = 0; i < 4; i++) net_our_ip[i] = ip[i];
    for (int i = 0; i < 4; i++) net_gw_ip[i] = gw[i];
    if (n >= 3 && ip4_parse_(w[2], dns)) net_dns_server_set(dns);
    net_arp_flush();
    vga_write("netcfg: ip/gw/dns updated, ARP cache cleared\n");
}

/* ---------- hwinfo: full hardware-properties dump ---------- */
static void cmd_hwinfo(void) {
    static char buf[3072];
    int n = hw_summary(buf, sizeof(buf));
    if (n <= 0) { vga_write("hwinfo: nothing\n"); return; }
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    vga_write(buf);
}

/* ---------- top: task monitor with live CPU% ---------- */
static void top_draw(uint32_t win_ticks) {
    struct toprow {
        int tid, uid, ring, state;
        uint32_t dt, pages;
        char name[16];
    } rows[MAX_TASKS];
    int nr = 0;
    uint32_t idle_now = task_idle_ticks();

    static uint32_t prev_ticks[MAX_TASKS], prev_idle;
    uint32_t now[MAX_TASKS];

    for (int slot = 0; slot < MAX_TASKS; slot++) {
        int tid; uint32_t ck, pg; int st, uid, ring; char nm[16];
        now[slot] = 0;
        if (!task_stat(slot, &tid, &ck, &pg, &st, &uid, &ring, nm, sizeof(nm)))
            continue;
        now[slot] = ck;
        rows[nr].tid = tid; rows[nr].uid = uid; rows[nr].ring = ring;
        rows[nr].state = st; rows[nr].pages = pg;
        rows[nr].dt = (ck >= prev_ticks[slot]) ? ck - prev_ticks[slot] : 0;
        {
            int i;
            for (i = 0; i < 15 && nm[i]; i++) rows[nr].name[i] = nm[i];
            rows[nr].name[i] = 0;
        }
        nr++;
    }

    /* insertion sort by delta desc */
    for (int i = 1; i < nr; i++) {
        struct toprow r = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].dt < r.dt) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = r;
    }

    vga_clear();
    vga_write("CatOS top - ");
    print_int((int)(task_uptime_ms() / 1000));
    vga_write("s up, ");
    print_int(hw_num(HW_CPU_MHZ));
    vga_write(" MHz, tasks ");
    print_int(hw_num(HW_TASKS));
    vga_write("\nmem ");
    print_int(hw_num(HW_MEM_USED_KB));
    vga_write("K used / ");
    print_int(hw_num(HW_MEM_FREE_KB));
    vga_write("K free / ");
    print_int(hw_num(HW_MEM_TOTAL_KB));
    vga_write("K total\n");

    if (win_ticks) {
        uint32_t idle_d = (idle_now >= prev_idle) ? idle_now - prev_idle : 0;
        int busy = 100 - (int)(idle_d * 100 / win_ticks);
        if (busy < 0) busy = 0;
        if (busy > 100) busy = 100;
        vga_write("cpu: ");
        print_int(busy);
        vga_write("% busy (");
        print_int((int)idle_d);
        vga_write("/");
        print_int((int)win_ticks);
        vga_write(" idle ticks)\n");
    } else {
        vga_write("cpu: (sampling, next frame in 2s)\n");
    }
    vga_write("TID  UID   RING  STATE   CPU%  PAGES  NAME\n");

    static const char *stn[] = {
        "UNUSED", "READY ", "RUN   ", "SLEEP ", "KB    ", "CHILD ", "ZOMBIE",
    };
    for (int i = 0; i < nr; i++) {
        print_int(rows[i].tid); vga_write(rows[i].tid < 10 ? "    " : "   ");
        print_int(rows[i].uid); vga_write(rows[i].uid < 10 ? "    " : "   ");
        vga_write(rows[i].ring == 0 ? "0    " : "3    ");
        vga_write(stn[rows[i].state <= 6 ? rows[i].state : 0]);
        vga_write(" ");
        if (win_ticks) {
            int pct = (int)(rows[i].dt * 100 / win_ticks);
            if (pct > 100) pct = 100;
            print_pad(pct, 3);
        } else {
            vga_write("  .");
        }
        vga_write("%  ");
        print_pad((int)rows[i].pages, 4);
        vga_write("  ");
        vga_write(rows[i].name);
        vga_write("\n");
    }
    vga_write("-- any key quits, refresh 2s --\n");

    for (int slot = 0; slot < MAX_TASKS; slot++) prev_ticks[slot] = now[slot];
    prev_idle = idle_now;
}

static void cmd_top(void) {
    top_draw(0);                             /* first frame: totals only */
    for (;;) {
        /* wait 2 s (20 ticks) or until a REAL key arrives; stale break
         * codes left in the queue by the launching readline are decoded
         * away and never count as the quit key */
        int k = kb_getkey_timed(20);
        if (k >= 0) break;
        top_draw(20);                        /* 20-tick sampling window */
    }
    vga_clear();
}

/* ---------- netdiag: dump NIC/driver state ---------- */
extern uint8_t net_mac[6];
extern int net_ready;
extern int netdbg_tx_calls, netdbg_tx_last, netdbg_rx_frames, netdbg_io_base;
extern int netdbg_io_width;
extern int      netdbg_pci_count;
extern uint16_t netdbg_pci_vend[8];
extern uint16_t netdbg_pci_dev[8];
extern uint16_t netdbg_csr0;
extern uint32_t netdbg_tx_word0, netdbg_rx_word0;
extern uint32_t netdbg_desc_tx0(void), netdbg_desc_rx0(void);
extern uint32_t netdbg_init_addr(void), netdbg_init_rxring(void), netdbg_init_txring(void);
extern uint16_t netdbg_csr(int i);
static void cmd_netdiag(const char *arg) {
    (void)arg;
    vga_write("PCI bus 0: ");
    if (netdbg_pci_count == 0) vga_write("no devices\n");
    for (int i = 0; i < netdbg_pci_count; i++) {
        vga_write("vend="); print_hex(netdbg_pci_vend[i]);
        vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
        if (i < netdbg_pci_count - 1) vga_write("  ");
    }
    vga_write("\n");
    vga_write("net_ready="); print_int(net_ready); vga_write("  io_base=0x");
    print_hex((uint32_t)netdbg_io_base);
    vga_write("  width="); print_int(netdbg_io_width); vga_write("-bit\n");
    vga_write("csr0=0x"); print_hex(netdbg_csr0); vga_write("\n");
    vga_write("mac="); print_hex((net_mac[0]<<8)|net_mac[1]); vga_write(":");
    print_hex((net_mac[2]<<8)|net_mac[3]); vga_write(":");
    print_hex((net_mac[4]<<8)|net_mac[5]); vga_write("\n");
    vga_write("tx_calls="); print_int(netdbg_tx_calls);
    vga_write(" tx_last="); print_int(netdbg_tx_last);
    vga_write(" rx_frames="); print_int(netdbg_rx_frames); vga_write("\n");
    vga_write("tx_word0=0x"); print_hex(netdbg_tx_word0);
    vga_write(" rx_word0=0x"); print_hex(netdbg_rx_word0); vga_write("\n");
    vga_write("desc_tx0=0x"); print_hex(netdbg_desc_tx0());
    vga_write(" desc_rx0=0x"); print_hex(netdbg_desc_rx0()); vga_write("\n");
    vga_write("init@0x"); print_hex(netdbg_init_addr());
    vga_write(" rxr=0x"); print_hex(netdbg_init_rxring());
    vga_write(" txr=0x"); print_hex(netdbg_init_txring()); vga_write("\n");
    vga_write("XMTRC="); print_int(netdbg_csr(40));
    vga_write(" CXDA=0x"); print_hex(netdbg_csr(34) | ((uint32_t)netdbg_csr(35) << 16));
    vga_write(" RL/TL="); print_int(netdbg_csr(76)); vga_write("/"); print_int(netdbg_csr(78)); vga_write("\n");
    vga_write("csr0_now=0x"); print_hex(netdbg_csr(0));
    vga_write(" csr15(mode)="); print_hex(netdbg_csr(15));
    vga_write(" POLL="); print_int(netdbg_csr(46)); vga_write("\n");
    vga_write("IADR=0x"); print_hex(netdbg_csr(1) | ((uint32_t)netdbg_csr(2) << 16));
    vga_write(" csr6="); print_hex(netdbg_csr(6));
    vga_write(" XMTRC="); print_int(netdbg_csr(74)); vga_write("\n");
    vga_write("init_rcvrl="); print_int(netdbg_init_rcvrl);
    vga_write(" init_xmtrl="); print_int(netdbg_init_xmtrl); vga_write("\n");
}

/* explain why the network is down, appended to "network not ready" */
static void net_why_down(void) {
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    if (netdbg_pci_count == 0) {
        vga_write(" (no PCI NIC; add -device pcnet)\n");
        return;
    }
    for (int i = 0; i < netdbg_pci_count; i++)
        if (netdbg_pci_vend[i] == 0x1022 && netdbg_pci_dev[i] == 0x2000) {
            vga_write(" (PCnet found but init failed; run 'netdiag')\n");
            return;
        }
    vga_write(" (NIC on bus is not PCnet 1022:2000; use -device pcnet)\n");
}

/* ---------- curl: one-shot HTTP GET ---------- */
static void cmd_curl(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: curl http://IP[:port]/path\n");
        vga_write("e.g.   curl http://10.0.2.2:8080/API.txt\n");
        return;
    }
    if (!net_up()) { vga_write("curl: network not ready"); net_why_down(); return; }
    static char buf[8192];
    int n = net_http_get(arg, buf, sizeof(buf));
    if (n < 0) {
        vga_write("curl: error ");
        print_int(n);
        vga_write(" (1=NIC 2=host 3=connect 4=http 5=resp 6=send)\n");
        return;
    }
    vga_write("--- ");
    vga_write(arg);
    vga_write(" ---\n");
    vga_write(buf);
    if (n > 0 && buf[n-1] != '\n') vga_write("\n");
    vga_write("--- "); print_int(n); vga_write(" bytes ---\n");
}

/* ---------- ping: ICMP echo ---------- */
static void cmd_ping(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: ping IP_or_host\n");
        return;
    }
    if (!net_up()) { vga_write("ping: network not ready"); net_why_down(); return; }
    for (int i = 0; i < 3; i++) {
        int ok = net_ping(arg);
        vga_write(ok == 0 ? "reply  " : "no-reply ");
        vga_write(arg);
        vga_write("\n");
        if (ok != 0) break;
        for (volatile long d = 0; d < 800000L; d++) { }  /* small gap between tries */
    }
}

/* ---------- dns: resolve a hostname ---------- */
static void cmd_dns(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { vga_write("usage: dns hostname\n"); return; }
    if (!net_up()) { vga_write("dns: network not ready"); net_why_down(); return; }
    uint8_t ip[4];
    if (net_dns_lookup(arg, ip) != 0) {
        vga_write("dns: could not resolve '");
        vga_write(arg);
        vga_write("'\n");
        return;
    }
    vga_write(arg);
    vga_write(" -> ");
    char t[4];
    for (int i = 0; i < 4; i++) {
        int d = ip[i], ti = 0;
        if (d == 0) { vga_putc('0'); }
        while (d > 0) { t[ti++] = (char)('0' + d % 10); d /= 10; }
        while (ti > 0) vga_putc(t[--ti]);
        if (i < 3) vga_putc('.');
    }
    vga_write("\n");
}

/* ---------- udp: send a datagram, then wait for one reply ----------
 * works with a host echo server, e.g.
 *   python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('',9999));
 *              while 1: d,a=s.recvfrom(2048); s.sendto(d,a)"
 */
static void cmd_udp(const char *arg) {
    while (*arg == ' ') arg++;
    char ip[64];
    int pi = 0;
    while (*arg && *arg != ' ' && pi < 63) ip[pi++] = *arg++;
    ip[pi] = 0;
    while (*arg == ' ') arg++;
    int port = 0;
    while (*arg >= '0' && *arg <= '9') { port = port * 10 + (*arg - '0'); arg++; }
    while (*arg == ' ') arg++;
    if (!*ip || port <= 0 || port > 65535 || !*arg) {
        vga_write("usage: udp IP port text\n");
        return;
    }
    if (!net_up()) { vga_write("udp: network not ready"); net_why_down(); return; }
    uint32_t tlen = 0;
    while (arg[tlen]) tlen++;
    if (net_udp_send(ip, (uint16_t)port, (const uint8_t *)arg, tlen) != 0) {
        vga_write("udp: send failed\n");
        return;
    }
    vga_write("sent, waiting for reply...\n");
    static char rbuf[2048];
    char rip[24];
    int rport = 0;
    /* poll the NIC while waiting (the old loop never did -> "no reply") */
    extern volatile uint32_t timer_ticks;
    extern void net_poll(void);
    uint32_t deadline = timer_ticks + 200;
    for (;;) {
        net_poll();
        int n = net_udp_recv(rip, &rport, (uint8_t *)rbuf, sizeof(rbuf) - 1);
        if (n > 0) {
            vga_write("reply from ");
            vga_write(rip);
            vga_write(":");
            print_int(rport);
            vga_write(" (");
            print_int(n);
            vga_write(" bytes): ");
            rbuf[n] = 0;
            vga_write(rbuf);
            vga_write("\n");
            return;
        }
        if ((int32_t)(timer_ticks - deadline) >= 0) break;
        task_maybe_yield();
        task_sleep_ticks(1);
    }
    vga_write("udp: no reply\n");
}

static void cmd_format(void) {    vga_write("Format FAT32 volume?  This erases ALL files.\n");
    vga_write("Disk: ");
    uint32_t tot = 0;
    ata_identify(&tot);
    if (tot == 0) tot = 131072;
    print_int((int)(tot / 2048));            /* MiB */
    vga_write(" MiB IDE, FAT32, 512B/sector ");
    uint32_t cs = fat_cluster_size();
    print_int((int)(cs ? cs / 512 : 8));
    vga_write(" sectors/cluster.\n");
    vga_write("Proceed? (y/n) ");
    char ans[16];
    kb_readline(ans, sizeof(ans));
    if (ans[0] == 'y' || ans[0] == 'Y') {
        fs_format();
        vga_write("FAT32 volume formatted.\n");
        vga_write("Top-level dirs: /dev /etc /bin /sbin /lib /usr /home\n");
        vga_write("                /root /tmp /var /proc /sys /mnt /boot\n");
        ce_install_builtins();      /* put /bin apps + /appdevelop docs on disk */
    } else {
        vga_write("Aborted.\n");
    }
}

/* ---------- nano-style full-screen editor ---------- */
#define ED_MAX_LINES 64
#define ED_MAX_COLS  79
#define ED_ROWS      23          /* content rows on screen (rows 1..23) */

static char ed_lines[ED_MAX_LINES][ED_MAX_COLS + 1];
static int  ed_nlines;
static int  ed_modified;
static int  ed_top;              /* first visible content line */
static int  ed_cx, ed_cy;        /* cursor column / content line */
static char ed_fname[FS_NAME_MAX];

static int ed_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ed_status_msg(const char *m) {
    vga_set_color(VGA_WHITE, VGA_LIGHT_BLUE);
    vga_write_at(0, 24, m);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void ed_load(fs_entry_t *f) {
    ed_nlines = 0;
    int n = fs_line_count(f);
    char lb[ED_MAX_COLS + 2];
    for (int i = 0; i < n && i < ED_MAX_LINES; i++) {
        if (fs_get_line(f, i, lb, sizeof(lb)) == 0) {
            int l = ed_strlen(lb);
            if (l > ED_MAX_COLS) l = ED_MAX_COLS;
            for (int j = 0; j < l; j++) ed_lines[ed_nlines][j] = lb[j];
            ed_lines[ed_nlines][l] = 0;
            ed_nlines++;
        }
    }
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    ed_modified = 0;
    ed_top = 0; ed_cx = 0; ed_cy = 0;
}

static int ed_save(fs_entry_t *f) {
    int total = 0;
    for (int i = 0; i < ed_nlines; i++) total += ed_strlen(ed_lines[i]) + 1;
    if (total > FS_DATA_MAX) return -1;
    fs_clear_file(f);
    for (int i = 0; i < ed_nlines; i++)
        if (fs_append_line(f, ed_lines[i]) != 0) return -1;
    if (fs_save(f) != 0) return -1;    /* write back to the FAT32 volume */
    ed_modified = 0;
    return 0;
}

static void ed_redraw(void) {
    int y, x;
    for (y = 1; y <= 23; y++)
        for (x = 0; x < 80; x++)
            vga_putc_at(x, y, ' ');

    /* status row */
    vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
    int sx = 0;
    vga_write_at(sx, 0, "CatOS nano  "); sx += 12;
    vga_write_at(sx, 0, ed_fname); sx += ed_strlen(ed_fname);
    vga_write_at(sx, 0, "  "); sx += 2;
    char tmp[24];
    itoa(ed_nlines, tmp, 0); vga_write_at(sx, 0, tmp); sx += ed_strlen(tmp);
    vga_write_at(sx, 0, " lines"); sx += 6;
    if (ed_modified) { vga_write_at(sx, 0, "  [modified]"); sx += 12; }

    /* content rows */
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int r = 0; r < ED_ROWS; r++) {
        int ln = ed_top + r;
        if (ln >= ed_nlines) break;
        vga_write_at(0, 1 + r, ed_lines[ln]);
    }

    /* bottom bar */
    vga_set_color(VGA_WHITE, VGA_LIGHT_BLUE);
    vga_write_at(0, 24, "^G Help  ^O Save  ^X Exit  ^K Del Line  ^U Clear  Arrows Move");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    int scr_y = 1 + (ed_cy - ed_top);
    if (scr_y < 1) scr_y = 1;
    if (scr_y > 23) scr_y = 23;
    vga_set_cursor(ed_cx, scr_y);
}

static void ed_insert(char c) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (l >= ED_MAX_COLS) { ed_status_msg("Line is full (max 79 chars)"); return; }
    for (int i = l; i > ed_cx; i--) ln[i] = ln[i - 1];
    ln[ed_cx] = c;
    ln[l + 1] = 0;
    ed_cx++;
}

static void ed_backspace(void) {
    char *ln = ed_lines[ed_cy];
    if (ed_cx > 0) {
        int l = ed_strlen(ln);
        for (int i = ed_cx - 1; i < l; i++) ln[i] = ln[i + 1];
        ed_cx--;
    } else if (ed_cy > 0) {
        char *prev = ed_lines[ed_cy - 1];
        int pl = ed_strlen(prev);
        char *cur = ed_lines[ed_cy];
        int cl = ed_strlen(cur);
        if (pl + cl > ED_MAX_COLS) cl = ED_MAX_COLS - pl;
        for (int i = 0; i < cl; i++) prev[pl + i] = cur[i];
        prev[pl + cl] = 0;
        for (int i = ed_cy; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
        ed_cy--;
        ed_cx = pl;
    }
}

static void ed_delete(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_cx < l) {
        for (int i = ed_cx; i < l; i++) ln[i] = ln[i + 1];
    } else if (ed_cy < ed_nlines - 1) {
        char *next = ed_lines[ed_cy + 1];
        int nl = ed_strlen(next);
        if (l + nl > ED_MAX_COLS) nl = ED_MAX_COLS - l;
        for (int i = 0; i < nl; i++) ln[l + i] = next[i];
        ln[l + nl] = 0;
        for (int i = ed_cy + 1; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
    }
}

static void ed_enter(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_nlines >= ED_MAX_LINES) { ed_status_msg("Too many lines (max 64)"); return; }
    for (int i = ed_nlines; i > ed_cy + 1; i--)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i - 1][j];
    int start = ed_cx;
    if (start > l) start = l;
    char *newln = ed_lines[ed_cy + 1];
    int nl = 0;
    for (int i = start; i < l; i++) {
        newln[nl++] = ln[i];
        if (nl > ED_MAX_COLS) break;
    }
    newln[nl] = 0;
    ln[start] = 0;
    ed_nlines++;
    ed_cy++;
    ed_cx = 0;
}

static void ed_del_line(void) {
    for (int i = ed_cy; i < ed_nlines - 1; i++)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
    ed_nlines--;
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1;
    if (ed_cx > ed_strlen(ed_lines[ed_cy])) ed_cx = ed_strlen(ed_lines[ed_cy]);
}

static void cmd_edit(const char *arg) {
    if (!*arg) { vga_write("usage: edit FILE\n"); return; }
    /* refuse files the 512-byte editor buffer cannot hold: loading used
     * to silently truncate them and ^O then overwrote the file */
    {
        uint32_t cl = 0, sz = 0;
        if (fs_resolve_file(arg, &cl, &sz) == 0 && sz >= FS_DATA_MAX) {
            vga_write("edit: file too large for the 512-byte editor;");
            vga_write(" refusing (a save would truncate it)\n");
            return;
        }
    }
    fs_entry_t *f = fs_create_file(arg);
    if (!f) { vga_write("edit: cannot create/open file\n"); return; }

    int fi = 0;
    while (arg[fi] && fi < FS_NAME_MAX - 1) { ed_fname[fi] = arg[fi]; fi++; }
    ed_fname[fi] = 0;

    ed_load(f);
    ed_redraw();

    for (;;) {
        int k = kb_getkey();

        if (k == 0x18) {                    /* ^X exit */
            int do_exit = 1;
            if (ed_modified) {
                ed_status_msg("Save?  ^O=yes  ^Y=no  any other=cancel");
                for (;;) {
                    k = kb_getkey();
                    if (k == 0x0F) { ed_save(f); break; }   /* ^O */
                    if (k == 0x19) { break; }                /* ^Y */
                    ed_redraw();
                    do_exit = 0;
                    break;
                }
                if (!do_exit) continue;
            }
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            vga_clear();
            vga_write("edit: saved ");
            print_int(fs_file_size(f));
            vga_write(" bytes.\n");
            return;
        }
        if (k == 0x0F || k == 0x13) {       /* ^O / ^S save */
            if (ed_save(f) != 0)
                ed_status_msg("SAVE FAILED: exceeds 512-byte file limit");
            else
                ed_status_msg("Saved.");
            continue;
        }
        if (k == 0x07) {                    /* ^G help */
            ed_status_msg("^O/^S save  ^X exit  ^K del line  ^U clear  arrows move  Del delete");
            continue;
        }
        if (k == 0x0B) { ed_del_line(); ed_modified = 1; ed_redraw(); continue; }  /* ^K */
        if (k == 0x15) {                    /* ^U clear all */
            ed_nlines = 1; ed_lines[0][0] = 0;
            ed_cy = 0; ed_cx = 0; ed_top = 0; ed_modified = 1; ed_redraw(); continue;
        }
        if (k == KB_UP)          { if (ed_cy > 0) ed_cy--; }
        else if (k == KB_DOWN)   { if (ed_cy < ed_nlines - 1) ed_cy++; }
        else if (k == KB_LEFT)   { if (ed_cx > 0) ed_cx--; }
        else if (k == KB_RIGHT)  { int l = ed_strlen(ed_lines[ed_cy]); if (ed_cx < l) ed_cx++; }
        else if (k == KB_HOME)   { ed_cx = 0; }
        else if (k == KB_END)    { ed_cx = ed_strlen(ed_lines[ed_cy]); }
        else if (k == KB_PGUP)   { ed_cy -= ED_ROWS; if (ed_cy < 0) ed_cy = 0; }
        else if (k == KB_PGDN)   { ed_cy += ED_ROWS; if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1; }
        else if (k == '\b')      { ed_backspace(); ed_modified = 1; }
        else if (k == KB_DEL)    { ed_delete(); ed_modified = 1; }
        else if (k == '\n')      { ed_enter(); ed_modified = 1; }
        else if (k >= 32 && k < 256) { ed_insert((char)k); ed_modified = 1; }

        int ll = ed_strlen(ed_lines[ed_cy]);
        if (ed_cx > ll) ed_cx = ll;
        if (ed_cy < ed_top) ed_top = ed_cy;
        if (ed_cy >= ed_top + ED_ROWS) ed_top = ed_cy - ED_ROWS + 1;
        ed_redraw();
    }
}

/* ---------- shell loop ---------- */
static char input[256];

static void prompt(void) {
    char pwd[160];
    fs_pwd(pwd, sizeof(pwd));
    vga_set_fg(VGA_LIGHT_GREEN);
    vga_write("catos");
    vga_set_fg(VGA_LIGHT_BLUE);
    vga_write(":");
    vga_write(pwd);
    vga_write("$ ");
    vga_set_fg(VGA_WHITE);
}

/* show the NIC state on the boot screen (kernel's net_init line is
 * cleared by vga_clear, so this re-prints it for the user) */
static void net_boot_status(void) {
    extern int net_ready;
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    if (net_ready) {
        vga_write("net: AMD PCnet-PCI up (10.0.2.15)\n");
        return;
    }
    vga_write("net: PCnet-PCI (1022:2000) NOT found; PCI bus 0: ");
    if (netdbg_pci_count == 0) {
        vga_write("no devices\n");
    } else {
        for (int i = 0; i < netdbg_pci_count; i++) {
            vga_write("vend="); print_hex(netdbg_pci_vend[i]);
            vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
            if (i < netdbg_pci_count - 1) vga_write(" ");
        }
        vga_write("\n");
    }
    vga_write("add to QEMU:  -netdev user,id=n0  -device pcnet,netdev=n0\n");
}

void shell_run(void) {
    mbi = (multiboot_info_t *)multiboot_info_ptr;

    vga_clear();
    fs_init();
    ce_init();
    fs_perm_seed();
    cmd_about();
    net_boot_status();
    if (!fs_fs_ready())
        vga_write("\n[!] No FAT32 volume on IDE drive.  Type 'format' first.\n");
    else
        ce_install_builtins();      /* fallback: install built-in apps at boot */


    while (1) {
        prompt();
        kb_readline(input, sizeof(input));

        /* Split first token */
        char *cmd = input;
        while (*cmd == ' ') cmd++;
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }

        if (!*cmd) continue;

        if (!strcmp_(cmd, "help"))            cmd_help();
        else if (!strcmp_(cmd, "clear"))      vga_clear();
        else if (!strcmp_(cmd, "echo"))      { vga_write(arg); vga_write("\n"); }
        else if (!strcmp_(cmd, "about"))      cmd_about();
        else if (!strcmp_(cmd, "uname"))      cmd_uname();
        else if (!strcmp_(cmd, "date"))       cmd_date();
        else if (!strcmp_(cmd, "mem"))        cmd_mem();
        else if (!strcmp_(cmd, "reboot"))    cmd_reboot();
        else if (!strcmp_(cmd, "hang"))      cmd_hang();
        else if (!strcmp_(cmd, "dir") || !strcmp_(cmd, "ls")) fs_list(*arg ? arg : 0);
        else if (!strcmp_(cmd, "cd"))         cmd_cd(arg);
        else if (!strcmp_(cmd, "mkdir"))      cmd_mkdir(arg);
        else if (!strcmp_(cmd, "touch"))      cmd_touch(arg);
        else if (!strcmp_(cmd, "format"))     cmd_format();
        else if (!strcmp_(cmd, "edit"))      cmd_edit(arg);
        else if (!strcmp_(cmd, "run"))        cmd_run(arg, UID_ROOT);
        else if (!strcmp_(cmd, "sudo")) {
            /* sudo CMD ...: run as root (uid 0) */
            while (*arg == ' ') arg++;
            char *scmd = arg;
            char *sarg = arg;
            while (*sarg && *sarg != ' ') sarg++;
            if (*sarg) { *sarg = 0; sarg++; while (*sarg == ' ') sarg++; }
            if (!*scmd) { vga_write("usage: sudo COMMAND [args...]\n"); }
            else if (!strcmp_(scmd, "run"))   cmd_run(sarg, UID_ROOT);
            else {
                char *argv[16];
                int argc = 1;
                argv[0] = scmd;
                argc += split_words(sarg, argv + 1, 15);
                if (run_from_path(scmd, argv, argc, 0, UID_ROOT) != 0) {
                    vga_write("catos: unknown command: ");
                    vga_write(scmd); vga_write("\n");
                }
            }
        }
        else if (!strcmp_(cmd, "ps"))          task_ps_print();
        else if (!strcmp_(cmd, "top"))         cmd_top();
        else if (!strcmp_(cmd, "hwinfo"))      cmd_hwinfo();
        else if (!strcmp_(cmd, "netcfg"))       cmd_netcfg(arg);
        else if (!strcmp_(cmd, "uptime")) {
            print_int((int)(task_uptime_ms() / 1000));
            vga_write("s (");
            print_int((int)task_count());
            vga_write(" tasks)\n");
        }
        else if (!strcmp_(cmd, "kill")) {
            int tid = 0, neg = 0;
            while (*arg == ' ') arg++;
            if (*arg == '-') { neg = 1; arg++; }
            while (*arg >= '0' && *arg <= '9') { tid = tid * 10 + (*arg - '0'); arg++; }
            if (neg) tid = -tid;
            if (tid == 0) { vga_write("usage: kill TID\n"); }
            else if (task_kill(tid, UID_ROOT) != 0)
                vga_write("kill: no such task\n");
        }
        else if (!strcmp_(cmd, "curl"))        cmd_curl(arg);
        else if (!strcmp_(cmd, "ping"))        cmd_ping(arg);
        else if (!strcmp_(cmd, "dns"))         cmd_dns(arg);
        else if (!strcmp_(cmd, "udp"))         cmd_udp(arg);
        else if (!strcmp_(cmd, "netdiag"))     cmd_netdiag(arg);
        else if (!strcmp_(cmd, "color")) {
            if (!*arg) {
                vga_write("usage: color 0..15  (note: 0 is black on black)\n");
            } else {
                int c = 0;
                while (*arg >= '0' && *arg <= '9') { c = c * 10 + (*arg - '0'); arg++; }
                if (c >= 0 && c <= 15) vga_set_fg(c);
                else vga_write("usage: color 0..15\n");
            }
        }
        else if (!strcmp_(cmd, "gfx"))        cmd_gfx();
        else if (!strcmp_(cmd, "text"))       cmd_text();
        else if (!strcmp_(cmd, "vdiag"))      vga_regdump();
        else {
            /* not a builtin: current-dir program, then /bin (PATH), else
             * report an unknown command.  Runs as uid USER (use 'sudo'
             * for root).  A trailing '&' backgrounds the task.  v2.5:
             * `cmd [> F | >> F | | CMD]` redirection is supported. */
            char *out_redir = 0, *pipe_to = 0; int append = 0;
            parse_redir(arg, &out_redir, &append, &pipe_to);
            char *argv[16];
            int argc = 1;
            argv[0] = cmd;
            argc += split_words(arg, argv + 1, 15);
            int bg = 0;
            if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
                bg = 1;
                argc--;
            }
            if (out_redir || pipe_to) {
                /* resolve the left-hand command via PATH, then spawn with
                 * redirection wired (run_from_path cannot wire it) */
                char p[FS_PATH_MAX];
                char pwd[FS_PATH_MAX];
                fs_pwd(pwd, sizeof(pwd));
                uint32_t c = 0, s = 0;
                p[0] = 0;
                if (!(pwd[0] == '/' && pwd[1] == 0)) {
                    if (strcat_b(p, sizeof(p), pwd) == 0) strcat_b(p, sizeof(p), "/");
                } else strcat_b(p, sizeof(p), "/");
                if (p[0]) strcat_b(p, sizeof(p), cmd);
                if (p[0]) strcat_b(p, sizeof(p), ".ce");
                if (p[0] && fs_resolve_file(p, &c, &s) != 0) {
                    p[0] = 0;
                    if (strcat_b(p, sizeof(p), "/bin/") == 0 &&
                        strcat_b(p, sizeof(p), cmd) == 0 &&
                        strcat_b(p, sizeof(p), ".ce") == 0 &&
                        fs_resolve_file(p, &c, &s) != 0) p[0] = 0;
                }
                if (!p[0]) {
                    vga_write("catos: unknown command: "); vga_write(cmd); vga_write("\n");
                } else if (pipe_to && *pipe_to) {
                    char *file2 = pipe_to;
                    char *sp = pipe_to;
                    while (*sp && *sp != ' ') sp++;
                    if (*sp) { *sp = 0; sp++; }
                    char *argv2[16];
                    int argc2 = 1;
                    argv2[0] = file2;
                    argc2 += split_words(sp, argv2 + 1, 15);
                    int h = kpipe_create();
                    if (h < 0) { vga_write("run: no pipe slots\n"); }
                    else {
                        vga_clear();
                        spawn_ce_redirect(p, argc, argv, 0, UID_USER, 0, 0, h, -1);
                        spawn_ce_redirect(file2, argc2, argv2, 1, UID_USER, 0, 0, -1, h);
                    }
                } else {
                    vga_clear();
                    spawn_ce_redirect(p, argc, argv, !bg, UID_USER,
                                      out_redir, append, -1, -1);
                }
            } else if (run_from_path(cmd, argv, argc, bg, UID_USER) != 0) {
                vga_write("catos: unknown command: ");
                vga_write(cmd); vga_write("\n");
            }
        }
    }
}
