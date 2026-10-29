#include "shell.h"
#include "vga.h"
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
        "  \\_____\\__,_|\\__|\\___|\\____/|_____/ \n"
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
        if (!append && fs_perm_check(ab, uid, PERM_WRITE) == 0)
            fs_write_raw_mode(ab, "", 0, 0);      /* pre-create/truncate */
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
