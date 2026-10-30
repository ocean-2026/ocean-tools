/* task.c - processes, the scheduler and the int 0x80 system-call layer.
 * See task.h for the design overview.
 *
 * Scheduling model (kernel non-preemptive, user preemptive):
 *   - ring0 code (shell, syscalls) never gets switched asynchronously;
 *     it yields cooperatively via task_yield()/task_block()
 *   - ring3 user code is preempted directly by the PIT ISR
 *     (sched_timer_tick swaps to another task's saved frame) and at
 *     system-call exits
 *   - when nothing is runnable the idle loop parks the current task and
 *     spins on hlt + net_poll until an interrupt makes someone runnable
 */

#include "task.h"
#include "io.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "fat.h"
#include "net.h"
#include "ce.h"
#include "hw.h"

extern void net_poll(void);
extern int  net_http_get(const char *url, char *out, int max);
extern int  net_dns_lookup(const char *host, uint8_t ip[4]);
extern int  net_udp_send(const char *ipstr, uint16_t port, const uint8_t *data, uint32_t len);
extern int  net_udp_recv(char *ipstr, int *port, uint8_t *buf, int max);
extern int  net_ping(const char *ipstr);
extern int  net_init(void);
extern int  net_up(void);
extern uint8_t net_our_ip[4];
extern volatile uint32_t timer_ticks;

/* ---- tiny helpers ------------------------------------------------------ */

static void w(const char *s) { vga_write(s); }

static void whex(uint32_t v) {
    static const char *h = "0123456789abcdef";
    char b[11]; b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0; w(b);
}

static void wdec(int v) {
    char b[12]; int i = 0, neg = 0;
    unsigned u = (unsigned)v;
    if (v < 0) { neg = 1; u = 0u - u; }
    if (u == 0) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    char o[13]; int j = 0;
    if (neg) o[j++] = '-';
    while (i) o[j++] = b[--i];
    o[j] = 0; w(o);
}

static int kstreq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int kstrneq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
    return 1;
}


/* ---- task table -------------------------------------------------------- */

static task_t  tasks[MAX_TASKS];
static task_t *cur_task;
static task_t  idle_task_v;              /* pseudo-task for the idle loop */
static uint8_t idle_stack[4096] __attribute__((aligned(16)));
static int     tasks_on;
static uint32_t idle_ticks_v;      /* ticks burned in the idle loop */
static volatile int need_resched;

/* one 8 KiB kernel stack per ring3 slot, in .bss */
static uint8_t task_kstacks[MAX_TASKS - 1][8192] __attribute__((aligned(16)));

/* v2.5: the big static ce_bounce[CE_MAX_IMAGE+32] (256 KiB .bss) is gone;
 * task_spawn_ce now walks the FAT chain directly into the mapped user
 * pages using a small cluster buffer (see task_spawn_ce). */

/* per-task directory listing snapshot for the ls syscalls */
typedef struct { char name[24]; uint32_t size; uint8_t isdir; } ls_ent_t;
#define LS_MAX 64
static ls_ent_t ls_buf[MAX_TASKS][LS_MAX];

static void idle_main(void);

task_t *task_current(void) { return cur_task; }
task_t *task_by_tid(int tid) {
    if (tid < 0 || tid >= MAX_TASKS) return 0;
    return tasks[tid].state != T_UNUSED ? &tasks[tid] : 0;
}
int tasks_ready(void) { return tasks_on; }
uint32_t task_uptime_ms(void) { return timer_ticks * 10; }
int console_owner_tid(void);
void console_wake_all(void);

void tasks_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].state = T_UNUSED;
    /* task 0 = the shell: ring0, running on the boot stack */
    tasks[0].tid = 0;
    tasks[0].state = T_RUNNING;
    tasks[0].uid = UID_ROOT;
    tasks[0].ring0 = 1;
    tasks[0].resume_frame = 0;
    tasks[0].parent = 0;
    tasks[0].space.pt = 0;
    tasks[0].space.dir = 0;
    tasks[0].name[0] = 's'; tasks[0].name[1] = 'h'; tasks[0].name[2] = 'e';
    tasks[0].name[3] = 'l'; tasks[0].name[4] = 'l'; tasks[0].name[5] = 0;
    cur_task = &tasks[0];

    /* the idle pseudo-task: ring0, parked on its own stack.  ksp MUST be
     * initialized - the first schedule_to(&idle) loads it directly into
     * esp, and a zero ksp would unwind the real-mode IVT as a stack. */
    idle_task_v.tid = -1;
    idle_task_v.state = T_READY;
    idle_task_v.uid = UID_ROOT;
    idle_task_v.ring0 = 1;
    idle_task_v.resume_frame = 0;
    idle_task_v.kstack_top = (uint32_t)idle_stack + sizeof(idle_stack);
    /* hand-built cpu_switch_to frame: pop edi/esi/ebx/ebp (zeros) then
     * ret -> idle_main.  Without this the first park into idle would
     * "return" from an empty stack. */
    *(uint32_t *)(idle_task_v.kstack_top - 4) = (uint32_t)idle_main;
    idle_task_v.ksp = idle_task_v.kstack_top - 20;
    idle_task_v.space.pt = 0;
    idle_task_v.space.dir = 0;
    idle_task_v.name[0] = 'i'; idle_task_v.name[1] = 'd'; idle_task_v.name[2] = 'l';
    idle_task_v.name[3] = 'e'; idle_task_v.name[4] = 0;

    tasks_on = 1;
}

/* ---- scheduler core ---------------------------------------------------- */

static task_t *pick_ready(void) {
    int start = cur_task ? cur_task->tid : 0;
    for (int k = 1; k <= MAX_TASKS; k++) {
        int i = (start + k) % MAX_TASKS;
        if (tasks[i].state == T_READY) return &tasks[i];
    }
    return 0;
}

/* enter `next` from ring0 context; never call from an ISR.
 * NOTE: this is intentionally NOT noreturn - the cpu_switch_to path
 * returns when this task is rescheduled later, and GCC must keep the
 * callers' return paths alive (a noreturn here made the rescheduled
 * task return into optimized-away frames -> jump to garbage). */
static void schedule_to(task_t *next) {
    task_t *prev = cur_task;
    cur_task = next;
    next->state = T_RUNNING;
    if (next->ring0 || !next->resume_frame) {
        prev->resume_frame = 0;          /* cooperative frame */
        if (!next->ring0) {
            tss_set_kstack(next->kstack_top);  /* its next trap must land
                                                  on ITS kernel stack    */
            cpu_cr3_write(next->space.dir);    /* and its USER pages must
                                                  map when it returns to
                                                  ring3 (e.g. after an
                                                  exit reset cr3 to kdir) */
        }
        cpu_switch_to(&prev->ksp, next->ksp);
        /* returned => we are `prev`, rescheduled onto our old stack */
        return;
    }
    /* park ourselves, then enter ring3 through its saved frame */
    /* v2.5 fix: the park path stores `prev` as a COOPERATIVE context
     * (cpu_switch_park_and_resume pushes callee-saved regs), so `prev`
     * must resume through cpu_switch_to, not through its old ISR frame. */
    prev->resume_frame = 0;
    cpu_switch_park_and_resume(&prev->ksp, next->ksp,
                               next->ring0 ? paging_kdir() : next->space.dir,
                               next->kstack_top);
}

static void schedule_to(task_t *next);

/* the idle entry: entered via a hand-built mini-frame the first time the
 * scheduler parks into idle (cpu_switch_to pops 4 regs + ret -> here).
 * Wakes on any interrupt (hlt), then hands the CPU to whoever is ready. */
static void idle_main(void) {
    for (;;) {

        need_resched = 0;
        if (net_up()) net_poll();
        cpu_idle_halt();                  /* wait for timer / keyboard */
        for (;;) {
            task_t *n = pick_ready();
            if (!n) break;
            schedule_to(n);               /* parks us; returns rescheduled */
        }
    }
}

static void reschedule(void) {
    task_t *n = pick_ready();
    if (n) { schedule_to(n); return; }
    /* nothing runnable: park into idle until an interrupt wakes someone */
    idle_task_v.state = T_READY;
    schedule_to(&idle_task_v);
    /* resumed: caller retries / continues */
}

void task_yield(void) {
    if (!tasks_on) return;
    if (!pick_ready()) return;
    cur_task->state = T_READY;
    reschedule();
}

void task_block(int state) {
    cur_task->state = state;
    reschedule();
}

void task_sleep_ticks(uint32_t ticks) {
    if (!tasks_on) { volatile uint32_t n = ticks * 20000u; while (n--) {} return; }
    cur_task->wake_tick = timer_ticks + ticks;
    task_block(T_SLEEPING);
}


static uint32_t maybe_ctr;
volatile uint32_t g_in_syscall = 0;   /* v2.5.1: no yield from ISR context */
void task_maybe_yield(void) {
    if (!tasks_on) return;
    if (g_in_syscall) return;                 /* ISR: pure busy-wait, do not reschedule */
    if (++maybe_ctr < 60000u) return;
    maybe_ctr = 0;
    task_yield();
}

/* timer IRQ hook (IF off): wake sleepers */
void task_tick_wake(void) {
    if (!tasks_on) return;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_SLEEPING &&
            (int32_t)(timer_ticks - tasks[i].wake_tick) >= 0)
            { tasks[i].state = T_READY; need_resched = 1; }
    }
}

/* timer stub hook: returns the esp of the frame to return into */
uint32_t sched_timer_tick(int_frame_t *f) {
    /* this ISR owns the EOI - it must go out BEFORE any iret path (a
     * missing EOI leaves IRQ0 in service -> immediate re-entry on iret
     * -> stack death spiral) */
    outb(0x20, 0x20);                    /* master EOI for IRQ0 */
    timer_ticks++;
    /* CPU accounting for `top`: every tick is charged to whoever is on
     * the CPU when it fires (the idle pseudo-task absorbs idle time) */
    if (cur_task && cur_task != &idle_task_v) cur_task->cpu_ticks++;
    else idle_ticks_v++;
    task_tick_wake();
    if (!tasks_on) return (uint32_t)f;
    task_t *c = cur_task;
    /* preempt only when the timer interrupted ring3 user code directly */
    if (!c || c->ring0 || c == &idle_task_v || (f->cs & 3) != 3) {
        need_resched = 1;
        return (uint32_t)f;
    }
    task_t *pick = 0;
    for (int k = 1; k <= MAX_TASKS && !pick; k++) {
        task_t *t = &tasks[(c->tid + k) % MAX_TASKS];
        if (t != c && t->state == T_READY && !t->ring0 && t->resume_frame)
            pick = t;
    }
    if (!pick) return (uint32_t)f;
    /* park the interrupted task: its frame is on its own kernel stack */
    c->ksp = (uint32_t)f;
    c->resume_frame = 1;
    c->state = T_READY;
    cur_task = pick;
    pick->state = T_RUNNING;
    cpu_cr3_write(pick->space.dir);
    tss_set_kstack(pick->kstack_top);
    return pick->ksp;                     /* stub swaps esp -> resumes pick */
}

/* ---- fault / exit ------------------------------------------------------- */

static void pipe_task_cleanup(task_t *t);   /* v2.5 (defined later) */
static void task_cleanup(task_t *t) {
    /* free the user address space (safe: t never runs again).  We may be
     * running on the dying task's kernel stack - only its USER frames are
     * freed here; the kernel stack is reused when the slot respawns. */
    cpu_cr3_write(paging_kdir());         /* never walk a dying directory */
    if (t->space.pt) task_space_destroy(&t->space);
    t->space.pt = 0;
    t->space.dir = 0;
    pipe_task_cleanup(t);                 /* v2.5: release pipe ends */
    if (console_owner_tid() == t->tid) console_wake_release(t->tid);
}

/* nobody is waiting for this task (background job): reclaim its slot
 * right away instead of leaking a zombie until the table fills up */
static void maybe_autoreap(task_t *t) {
    if (!t->parent || t->parent->state != T_WAIT_CHILD)
        t->state = T_UNUSED;
}

void task_die_fault(int_frame_t *f) {
    task_t *t = cur_task;
    w("\n[task "); wdec(t->tid); w(" '"); w(t->name);
    w("' killed: "); w(f->intno == 14 ? "page fault" : "fault"); w(" at eip=");
    whex(f->eip); w(" cr2="); whex(cpu_cr2_read()); w("]\n");
    task_cleanup(t);
    t->exit_code = -1;
    t->state = T_ZOMBIE;
    if (t->parent && t->parent->state == T_WAIT_CHILD) t->parent->state = T_READY;
    maybe_autoreap(t);
    reschedule();
    __builtin_unreachable();
}

void task_exit_current(int code) {
    task_t *t = cur_task;
    task_cleanup(t);
    t->exit_code = code;
    t->state = T_ZOMBIE;
    w("[task "); wdec(t->tid); w(" '"); w(t->name); w("' exit(");
    wdec(code); w(")]\n");
    if (t->parent && t->parent->state == T_WAIT_CHILD) t->parent->state = T_READY;
    maybe_autoreap(t);
    reschedule();
    __builtin_unreachable();
}

int task_kill(int tid, int by_uid) {
    task_t *t = task_by_tid(tid);
    if (!t || t == cur_task) return -1;
    if (by_uid != UID_ROOT && t->uid != by_uid) return -2;  /* not yours */
    if (t->state == T_ZOMBIE) return 0;
    task_cleanup(t);                      /* switches cr3 to the kernel dir */
    t->exit_code = -9;
    t->state = T_ZOMBIE;
    if (t->parent && t->parent->state == T_WAIT_CHILD) t->parent->state = T_READY;
    maybe_autoreap(t);
    /* the killer keeps the CPU: switch cr3 back to our own space */
    if (cur_task->space.dir)
        cpu_cr3_write(cur_task->space.dir);
    w("[kill: tid "); wdec(tid); w(" ("); w(t->name); w(")]\n");
    return 0;
}

void task_wait_child(task_t *child) {
    while (child->state == T_ZOMBIE || child->state == T_UNUSED ? 0 : 1)
        task_block(T_WAIT_CHILD);
    child->state = T_UNUSED;              /* reaped */
}

int task_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != T_UNUSED) n++;
    return n;
}

void task_ps_print(void) {
    static const char *names[] = {
        "UNUSED", "READY ", "RUN   ", "SLEEP ", "KB    ", "CHILD ", "ZOMBIE",
    };
    w("TID  UID   RING  STATE   NAME\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_UNUSED) continue;
        wdec(i); w(i < 10 ? "    " : "   ");
        wdec(tasks[i].uid); w(tasks[i].uid < 10 ? "    " : "   ");
        w(tasks[i].ring0 ? "0    " : "3    ");
        w(names[tasks[i].state <= T_ZOMBIE ? tasks[i].state : 0]); w("  ");
        w(tasks[i].name); w("\n");
    }
    w("--- "); wdec(task_count()); w(" task(s), uptime ");
    wdec((int)(task_uptime_ms() / 1000)); w("s ---\n");
}

/* ---- console input ownership ------------------------------------------- */

static volatile int console_owner = TID_SHELL;

int console_try_own(void) {
    if (console_owner == cur_task->tid) return 1;
    if (console_owner < 0) { console_owner = cur_task->tid; return 1; }
    return 0;
}

int console_owned_by_current(void) { return console_owner == cur_task->tid; }

void console_release(void) {
    if (console_owner == cur_task->tid) console_owner = -1;
}

int console_owner_tid(void) { return console_owner; }

void console_wake_all(void) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == T_WAIT_KB) { tasks[i].state = T_READY; need_resched = 1; }
    /* FIX(2026-10-27): 不能在键盘 ISR 里调用 reschedule()/schedule_to()——
       schedule_to 注释明确 "never call from an ISR"：ISR 内切换任务会破坏
       中断帧（ring3 CE 任务被唤醒后无法恢复、getch 永远读不到键）。
       need_resched 改由安全路径消费：syscall 返回前 preempt_at_syscall_exit，
       或 idle_main 循环 pick_ready。 */
}

void console_wake_release(int tid) {
    if (console_owner == tid) console_owner = -1;
}

/* ---- vsyscall page ------------------------------------------------------
 * Hand-assembled ring3 thunks: load cdecl args into ebx/ecx/edx/esi,
 * syscall number into eax, int 0x80, ret.  The table at VS_TABLE has the
 * exact member order of catos_api_t, so UNMODIFIED .ce binaries work:
 * their api->fn(...) call lands in a thunk -> int 0x80 -> the kernel.
 */

/* page layout (4096 bytes): generic thunks first (37 x <=32 B = 1184 B,
 * so the exit stub must NOT sit at 0x300 - the first build overflowed the
 * thunk emission into it and a `return 0` from ce_main jumped into
 * thunk code that never exits). */
#define VSYSCALL_BASE   USER_VSYSCALL      /* 0x2080000 */
#define VS_EXIT_STUB    (VSYSCALL_BASE + 0x500)
#define VS_LS_THUNK     (VSYSCALL_BASE + 0x600)
#define VS_TABLE        (VSYSCALL_BASE + 0x800)
#define VS_LS_H         (VSYSCALL_BASE + 0xC00)
#define VS_LS_NAME      (VSYSCALL_BASE + 0xC20)
#define VS_LS_SIZE      (VSYSCALL_BASE + 0xC40)

enum {
    SYS_WRITE = 0, SYS_PUTC, SYS_CLS, SYS_READLINE, SYS_TIME_STR,
    SYS_FS_READY, SYS_LS, SYS_TOUCH, SYS_MKDIR, SYS_CAT, SYS_SAVE,
    SYS_ALLOC, SYS_FREE, SYS_EXIT,
    SYS_PRINT_INT, SYS_PRINT_HEX, SYS_SET_FG, SYS_DELAY,
    SYS_ATOI, SYS_STRLEN, SYS_STRCMP, SYS_STRCPY, SYS_GETCH,
    SYS_FILE_EXISTS, SYS_FSIZE,
    SYS_NET_INIT, SYS_NET_UP, SYS_HTTP_GET,
    SYS_NET_DNS, SYS_NET_UDP_SEND, SYS_NET_UDP_RECV, SYS_NET_PING,
    SYS_NET_IP,
    SYS_GETUID, SYS_GETTID, SYS_KILL, SYS_YIELD,
    SYS_HW_NUM, SYS_HW_STR,                  /* v2.4 hardware properties */
    /* v2.5: big files / pipes / processes (appended at the END so old
     * binaries indexing only the first 33 api slots keep working) */
    SYS_FILE_READ, SYS_FILE_WRITE, SYS_FILE_DELETE,
    SYS_PIPE_CREATE, SYS_PIPE_WRITE, SYS_PIPE_READ, SYS_PIPE_CLOSE,
    SYS_SPAWN, SYS_WAIT,
    SYS_COUNT
};
#define SYS_LS_NEXT 1000

typedef struct { uint8_t *p; } emit_t;

static void e8(emit_t *e, uint8_t b) { *e->p++ = b; }
static void e32(emit_t *e, uint32_t v) {
    *e->p++ = (uint8_t)v; *e->p++ = (uint8_t)(v >> 8);
    *e->p++ = (uint8_t)(v >> 16); *e->p++ = (uint8_t)(v >> 24);
}

static void generic_thunk(emit_t *e, uint32_t idx, int nargs) {
    /* The old ring0 ABI was a plain cdecl call: programs keep EBX/ESI/EDI
     * (callee-saved) live ACROSS api calls - e.g. the api table pointer
     * itself usually sits in EBX for the whole ce_main.  The thunk must
     * therefore save/restore every register it uses for argument passing
     * (only EAX, the cdecl return register, may be clobbered). */
    e8(e,0x53);                          /* push ebx         */
    e8(e,0x51);                          /* push ecx         */
    e8(e,0x52);                          /* push edx         */
    e8(e,0x56);                          /* push esi         */
    if (nargs >= 1) { e8(e,0x8B); e8(e,0x5C); e8(e,0x24); e8(e,0x14); } /* mov ebx,[esp+14] */
    if (nargs >= 2) { e8(e,0x8B); e8(e,0x4C); e8(e,0x24); e8(e,0x18); } /* mov ecx,[esp+18] */
    if (nargs >= 3) { e8(e,0x8B); e8(e,0x54); e8(e,0x24); e8(e,0x1C); } /* mov edx,[esp+1C] */
    if (nargs >= 4) { e8(e,0x8B); e8(e,0x74); e8(e,0x24); e8(e,0x20); } /* mov esi,[esp+20] */
    e8(e,0xB8); e32(e, idx);             /* mov eax, idx     */
    e8(e,0xCD); e8(e,0x80);              /* int 0x80         */
    e8(e,0x5E);                          /* pop esi          */
    e8(e,0x5A);                          /* pop edx          */
    e8(e,0x59);                          /* pop ecx          */
    e8(e,0x5B);                          /* pop ebx          */
    e8(e,0xC3);                          /* ret              */
}

static void build_vsyscall_page(uint8_t *page) {
    emit_t e; e.p = page;

    /* exit trampoline at +0x300: a ce_main() that plain `return`s lands
     * here (the dummy return address pushed at spawn) */
    e.p = page + 0x500;
    e8(&e,0xB8); e32(&e,SYS_EXIT);       /* mov eax,SYS_EXIT */
    e8(&e,0x31); e8(&e,0xDB);            /* xor ebx,ebx      */
    e8(&e,0xCD); e8(&e,0x80);

    /* generic thunks 0x000.. : argc per catos_api_t member */
    static const uint8_t nargs[SYS_COUNT] = {
        /*0*/1, /*1*/1, /*2*/0, /*3*/2, /*4*/1,
        /*5*/0, /*6*/3, /*7*/1, /*8*/1, /*9*/3, /*10*/3,
        /*11*/1, /*12*/1, /*13*/1,
        /*14*/1, /*15*/1, /*16*/1, /*17*/1,
        /*18*/1, /*19*/1, /*20*/2, /*21*/2, /*22*/0,
        /*23*/1, /*24*/1,
        /*25*/0, /*26*/0, /*27*/3,
        /*28*/2, /*29*/4, /*30*/4, /*31*/1, /*32*/1,
        /*33*/0, /*34*/0, /*35*/2, /*36*/0,
        /*37*/1, /*38*/3,
        /*39*/4, /*40*/4, /*41*/1,             /* file read/write/delete */
        /*42*/0, /*43*/3, /*44*/3, /*45*/1,     /* pipe create/write/read/close */
        /*46*/3, /*47*/1,                       /* spawn / wait */
    };
    uint32_t taddr[SYS_COUNT];
    e.p = page;
    for (uint32_t i = 0; i < SYS_COUNT; i++) {
        taddr[i] = VSYSCALL_BASE + (uint32_t)(e.p - page);
        generic_thunk(&e, i, nargs[i]);
    }

    /* ls thunk at +0x400: ls(path, cb, ud) via LS_OPEN + LS_NEXT loop */
    e.p = page + 0x600;
    {
        e8(&e,0x53); e8(&e,0x52);                                 /* push ebx; push edx */
        e8(&e,0x8B); e8(&e,0x5C); e8(&e,0x24); e8(&e,0x0C);       /* mov ebx,[esp+0C] */
        e8(&e,0xB8); e32(&e,SYS_LS); e8(&e,0xCD); e8(&e,0x80);    /* LS_OPEN(path)   */
        e8(&e,0x85); e8(&e,0xC0);                                 /* test eax,eax    */
        int js_pos  = (int)(e.p - page); e8(&e,0x7C); e8(&e,0x00);/* js fail (patch) */
        e8(&e,0xA3); e32(&e,VS_LS_H);                             /* mov [h],eax     */
        int loop_pos = (int)(e.p - page);
        e8(&e,0xB8); e32(&e,SYS_LS_NEXT);                         /* mov eax,NEXT    */
        e8(&e,0x8B); e8(&e,0x1D); e32(&e,VS_LS_H);                /* mov ebx,[h]     */
        e8(&e,0xCD); e8(&e,0x80);
        e8(&e,0x85); e8(&e,0xC0);                                 /* test eax,eax    */
        int jle_pos = (int)(e.p - page); e8(&e,0x7E); e8(&e,0x00);/* jle done (patch)*/
        e8(&e,0xFF); e8(&e,0x74); e8(&e,0x24); e8(&e,0x14);       /* push [esp+14] ud*/
        e8(&e,0xFF); e8(&e,0x35); e32(&e,VS_LS_SIZE);             /* push [size]     */
        e8(&e,0x68); e32(&e,VS_LS_NAME);                          /* push name       */
        e8(&e,0x52);                                              /* push edx (dir)  */
        e8(&e,0xFF); e8(&e,0x54); e8(&e,0x24); e8(&e,0x20);       /* call [esp+20] cb*/
        e8(&e,0x83); e8(&e,0xC4); e8(&e,0x10);                    /* add esp,16      */
        e8(&e,0xEB);
        e8(&e,(uint8_t)(loop_pos - ((int)(e.p - page) + 1)));     /* jmp loop        */
        int done_pos = (int)(e.p - page);
        e8(&e,0x5A); e8(&e,0x5B); e8(&e,0xB8); e32(&e,0);         /* pop edx; pop ebx; mov eax,0 */
        e8(&e,0xC3);
        int fail_pos = (int)(e.p - page);
        e8(&e,0x5A); e8(&e,0x5B); e8(&e,0xB8); e32(&e,0xFFFFFFFFu);
        e8(&e,0xC3);
        page[js_pos  + 1] = (uint8_t)(fail_pos - (js_pos  + 2));
        page[jle_pos + 1] = (uint8_t)(done_pos - (jle_pos + 2));
    }
    taddr[SYS_LS] = VS_LS_THUNK;          /* api->ls -> special thunk */

    /* api pointer table at +0x800 (order == catos_api_t members: the
     * first 33 are the classic api, then v2.3 task extensions 33-38
     * (getuid at 33), then 9 v2.5 big-file/pipe/process slots 39-47) */
    e.p = page + 0x800;
    for (int i = 0; i < SYS_COUNT; i++) e32(&e, taddr[i]);
}

/* ---- permissions (security pillar) -------------------------------------- */

#define PERM_MAX 96
typedef struct { char path[FS_PATH_MAX]; uint16_t uid; uint16_t mode; } perm_t;
static perm_t perm_tab[PERM_MAX];
static int    perm_n;

void fs_perm_set(const char *path, int uid, int mode) {
    for (int i = 0; i < perm_n; i++)
        if (kstreq(perm_tab[i].path, path)) {
            perm_tab[i].uid = (uint16_t)uid; perm_tab[i].mode = (uint16_t)mode;
            return;
        }
    if (perm_n >= PERM_MAX) return;
    int i;
    for (i = 0; path[i] && i < FS_PATH_MAX - 1; i++) perm_tab[perm_n].path[i] = path[i];
    perm_tab[perm_n].path[i] = 0;
    perm_tab[perm_n].uid = (uint16_t)uid;
    perm_tab[perm_n].mode = (uint16_t)mode;
    perm_n++;
}

static int perm_default_write(const char *path) {
    /* unwritten files/dirs are root-owned 0644/0755; the world-writable
     * scratch areas are whitelisted here */
    if (!kstrneq(path, "/home/", 6) && !kstreq(path, "/home") &&
        !kstrneq(path, "/tmp/", 5) && !kstreq(path, "/tmp")) return 0;
    return 1;
}

int fs_perm_check(const char *path, int uid, int need) {
    /* v2.5: need is a bitmask of PERM_READ/PERM_WRITE/PERM_EXEC.
     * owner bits live in the mode's 6-7-8 positions, others in 0-1-2. */
    if (uid == UID_ROOT) return 0;
    for (int i = 0; i < perm_n; i++) {
        if (kstreq(perm_tab[i].path, path)) {
            if (perm_tab[i].uid == (uint16_t)uid)
                return (perm_tab[i].mode & (need << 6)) ? 0 : -1;
            return (perm_tab[i].mode & need) ? 0 : -1;
        }
    }
    /* no explicit entry: read/exec default allowed (0644/0755 semantics),
     * writes go through the world-writable whitelist */
    if (need == PERM_WRITE) return perm_default_write(path) ? 0 : -1;
    return 0;
}

void fs_perm_seed(void) {
    fs_perm_set("/home", UID_ROOT, 0777);
    fs_perm_set("/tmp", UID_ROOT, 0777);
    fs_perm_set("/bin", UID_ROOT, 0755);
    fs_perm_set("/appdevelop", UID_ROOT, 0755);
    fs_perm_set("/etc", UID_ROOT, 0755);
}

/* ---- user memory helpers ------------------------------------------------ */

static int u2k_str(char *dst, const char *us, int max) {
    if ((uint32_t)us < USER_IMAGE_BASE || (uint32_t)us >= USER_ARENA_END) return -1;
    for (int i = 0; i < max - 1; i++) {
        dst[i] = us[i];
        if (!us[i]) return 0;
        if ((uint32_t)&us[i] + 1 >= USER_ARENA_END) return -1;
    }
    dst[max - 1] = 0;
    return 0;
}

/* write through the task's page table (page by page, kernel alias) */
static void k2u_copy(task_space_t *s, uint32_t dst, const void *src, uint32_t len) {
    uint8_t *sp = (uint8_t *)src;
    while (len) {
        uint32_t pa = task_virt_to_phys(s, dst & ~0xFFFu);
        if (!pa) {
            pa = (uint32_t)kzalloc_frame();
            task_map_page(s, dst & ~0xFFFu, pa, 1);
        }
        uint32_t off = dst & 0xFFF;
        uint32_t n = 4096 - off; if (n > len) n = len;
        uint8_t *d = (uint8_t *)(pa + off);
        for (uint32_t i = 0; i < n; i++) d[i] = sp[i];
        dst += n; sp += n; len -= n;
    }
}

/* ---- per-task heap (SYS_ALLOC / SYS_FREE) -------------------------------- */

typedef struct heap_hdr { uint32_t size; uint32_t next; } heap_hdr_t;

static int heap_ensure(task_t *t, uint32_t vaddr) {
    if (vaddr < USER_HEAP_BASE || vaddr >= USER_HEAP_BASE + USER_HEAP_MAX) return -1;
    if (task_virt_to_phys(&t->space, vaddr & ~0xFFFu)) return 0;
    uint32_t f = (uint32_t)kzalloc_frame();
    if (!f) return -1;
    return task_map_page(&t->space, vaddr & ~0xFFFu, f, 1);
}

/* freelist head kept in the kernel (task_t has no spare field: use a
 * static table indexed by tid) */
static uint32_t heap_free_root[MAX_TASKS];

static void *sys_alloc(task_t *t, uint32_t size) {
    if (size == 0) size = 1;
    size = (size + 7u) & ~7u;
    if (heap_ensure(t, t->heap_end + sizeof(heap_hdr_t) + 8) != 0) return 0;
    heap_hdr_t *base = (heap_hdr_t *)t->heap_end;
    if (base->size == 0) {               /* first alloc: claim whole arena */
        base->size = USER_HEAP_MAX - (t->heap_end - USER_HEAP_BASE)
                     - sizeof(heap_hdr_t);
        base->next = 0;
        heap_free_root[t->tid] = t->heap_end;
    }
    uint32_t *pp = &heap_free_root[t->tid];
    while (*pp) {
        heap_hdr_t *h = (heap_hdr_t *)*pp;
        if (h->size >= size) {
            if (h->size >= size + sizeof(heap_hdr_t) + 8) {
                heap_hdr_t *rest = (heap_hdr_t *)((uint8_t *)h + sizeof(heap_hdr_t) + size);
                if (heap_ensure(t, (uint32_t)rest + sizeof(heap_hdr_t)) != 0) return 0;
                rest->size = h->size - size - sizeof(heap_hdr_t);
                rest->next = h->next;
                *pp = (uint32_t)rest;
                h->size = size;
            } else {
                *pp = h->next;
            }
            /* ensure every page of the payload: heap_ensure() above only
             * mapped the page holding the header, so any allocation whose
             * contents cross into the second page faulted on first use
             * (observed: cc.ce with >170 tokens -> #PF at the token
             * buffer's second page).  Map the full payload here. */
            {
                uint32_t p0 = ((uint32_t)h + sizeof(heap_hdr_t)) & ~0xFFFu;
                uint32_t p1 = ((uint32_t)h + sizeof(heap_hdr_t) + size - 1) & ~0xFFFu;
                for (uint32_t a = p0; a <= p1; a += 4096)
                    if (heap_ensure(t, a) != 0) return 0;
            }
            return (void *)((uint8_t *)h + sizeof(heap_hdr_t));
        }
        pp = &h->next;
    }
    return 0;
}

static void sys_free(task_t *t, void *p) {
    uint32_t a = (uint32_t)p;
    if (a < USER_HEAP_BASE + sizeof(heap_hdr_t) ||
        a >= USER_HEAP_BASE + USER_HEAP_MAX) return;
    heap_hdr_t *h = (heap_hdr_t *)(a - sizeof(heap_hdr_t));
    h->next = heap_free_root[t->tid];
    heap_free_root[t->tid] = (uint32_t)h;
}

/* ---- ls snapshot ---------------------------------------------------------- */

struct ls_fill { ls_ent_t *ents; int n; };
static void ls_fill_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    struct ls_fill *f = (struct ls_fill *)ud;
    if (f->n >= LS_MAX) return;
    int i;
    for (i = 0; name[i] && i < 23; i++) f->ents[f->n].name[i] = name[i];
    f->ents[f->n].name[i] = 0;
    f->ents[f->n].size = size;
    f->ents[f->n].isdir = (uint8_t)is_dir;
    f->n++;
}

/* ---- v2.5 pipes: kernel FIFOs for CE-to-CE communication --------------
 * handle = 0x100 + slot.  Blocking semantics: PIPE_WRITE waits for free
 * space, PIPE_READ waits for data; a closed peer end makes the blocked
 * side return.  Shell redirection uses a task's out_pipe / in_pipe. */
#define PIPE_MAX 8
#define PIPE_BUF 4096
typedef struct {
    uint8_t  buf[PIPE_BUF];
    uint16_t head, tail, len;
    uint8_t  open_w, open_r;
    uint8_t  used;
} pipe_t;
static pipe_t pipes[PIPE_MAX];


static pipe_t *pipe_by_handle(int h) {
    int idx = h - 0x100;
    if (idx < 0 || idx >= PIPE_MAX) return 0;
    return pipes[idx].used ? &pipes[idx] : 0;
}

static void pipe_task_cleanup(task_t *t) {
    if (t->out_pipe >= 0) {
        pipe_t *p = pipe_by_handle(t->out_pipe);
        if (p) p->open_w = 0;
        t->out_pipe = -1;
    }
    if (t->in_pipe >= 0) {
        pipe_t *p = pipe_by_handle(t->in_pipe);
        if (p) p->open_r = 0;
        t->in_pipe = -1;
    }
    for (int i = 0; i < PIPE_MAX; i++)
        if (pipes[i].used && !pipes[i].open_w && !pipes[i].open_r) {
            pipes[i].used = 0; pipes[i].len = 0;
        }
}

/* ring0 helpers for the shell: create a pipe and wire a task's ends */
int kpipe_create(void) {
    int idx = -1;
    for (int i = 0; i < PIPE_MAX; i++) if (!pipes[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    for (int k = 0; k < (int)sizeof(pipe_t); k++) ((uint8_t *)&pipes[idx])[k] = 0;
    pipes[idx].used = 1;
    pipes[idx].open_w = 1;
    pipes[idx].open_r = 1;
    return 0x100 + idx;
}

void kpipe_assign_out(int tid, int h) {
    task_t *t = task_by_tid(tid);
    if (t) t->out_pipe = h;
}

void kpipe_assign_in(int tid, int h) {
    task_t *t = task_by_tid(tid);
    if (t) t->in_pipe = h;
}

/* ---- the syscall dispatcher ---------------------------------------------- */

static int sys_touch_uid(const char *upath, int uid) {
    char p[FS_PATH_MAX];
    if (u2k_str(p, upath, sizeof(p)) != 0) return -3;
    if (fs_perm_check(p, uid, PERM_WRITE) != 0) return -13;           /* EACCES */
    uint32_t c, s;
    int existed = fs_resolve_file(p, &c, &s) == 0;
    int r = fs_touch(p);
    if (r == FS_OK && !existed) fs_perm_set(p, uid, 0644);
    return r;
}

static int sys_mkdir_uid(const char *upath, int uid) {
    char p[FS_PATH_MAX];
    if (u2k_str(p, upath, sizeof(p)) != 0) return -3;
    if (fs_perm_check(p, uid, PERM_WRITE) != 0) return -13;
    int r = fs_mkdir(p);
    if (r == FS_OK) fs_perm_set(p, uid, 0755);
    return r;
}

static int sys_save_uid(const char *upath, const char *udata, uint32_t size, int uid) {
    char p[FS_PATH_MAX];
    static char buf[FS_DATA_MAX];
    if (size > FS_DATA_MAX) return -7;
    if (u2k_str(p, upath, sizeof(p)) != 0) return -3;
    if ((uint32_t)udata < USER_IMAGE_BASE || (uint32_t)udata + size > USER_ARENA_END) return -3;
    if (fs_perm_check(p, uid, PERM_WRITE) != 0) return -13;
    for (uint32_t i = 0; i < size; i++) buf[i] = udata[i];
    uint32_t c, s;
    int existed = fs_resolve_file(p, &c, &s) == 0;
    int r = fs_write_text(p, buf, size);
    if (r == FS_OK && !existed) fs_perm_set(p, uid, 0644);
    return r;
}

static void preempt_at_syscall_exit(int_frame_t *f) {
    /* the frame on this kernel stack doubles as the saved context; the
     * task resumes via cpu_frame_resume into ring3 later, with the
     * syscall return value already stored in frame->eax */
    task_t *pick = 0;
    for (int k = 1; k <= MAX_TASKS && !pick; k++) {
        task_t *tt = &tasks[(cur_task->tid + k) % MAX_TASKS];
        if (tt != cur_task && tt->state == T_READY && !tt->ring0 && tt->resume_frame)
            pick = tt;
    }
    if (!pick) { need_resched = 0; return; }
    task_t *me = cur_task;
    me->ksp = (uint32_t)f;
    me->resume_frame = 1;
    me->state = T_READY;
    cur_task = pick;
    pick->state = T_RUNNING;
    cpu_frame_resume(pick->ksp, pick->space.dir, pick->kstack_top);
}

void isr_syscall(int_frame_t *f) {
    uint32_t nr = f->eax;
    uint32_t a1 = f->ebx, a2 = f->ecx, a3 = f->edx, a4 = f->esi, a5 = f->edi;
    task_t *t = cur_task;
    int uid = t->uid;
    uint32_t ret = (uint32_t)-8;               /* ENOSYS */
    /* v2.5 fix: a fresh trap means our execution context now lives in the
     * frame on this kernel stack.  The spawn-time/first-entry frame and any
     * stale ISR frame pointer are no longer valid for a later frame-resume;
     * rescheduling from now on goes through the cooperative path or a new
     * preempt frame captured below. */
    t->resume_frame = 0;
    g_in_syscall++;

    switch (nr) {
    case SYS_WRITE: {
        static char b[514];
        const char *s = (const char *)a1;
        if ((uint32_t)s < USER_IMAGE_BASE || (uint32_t)s >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        int i = 0;
        while (s[i] && i < 512) { b[i] = s[i]; i++; }
        b[i] = 0;
        /* v2.5 redirection: pipe target first, then file target, else vga */
        if (t->out_pipe >= 0) {
            pipe_t *pp = pipe_by_handle(t->out_pipe);
            if (!pp) { ret = (uint32_t)-1; break; }
            int wrote = 0; int perr = 0;
            while (wrote < i) {
                if (!pp->open_r) { perr = -1; break; }
                int free = PIPE_BUF - 1 - pp->len;
                if (free <= 0) { task_sleep_ticks(1); continue; }
                int n = i - wrote; if (n > free) n = free;
                for (int k = 0; k < n; k++) {
                    pp->buf[pp->tail] = b[wrote + k];
                    pp->tail = (uint16_t)((pp->tail + 1) % PIPE_BUF);
                }
                pp->len = (uint16_t)(pp->len + n);
                wrote += n;
            }
            ret = perr ? (uint32_t)perr : (uint32_t)i;
            break;
        }
        if (t->out_file[0]) {
            /* write to the redirect file (append or truncate-replace) */
            if (fs_perm_check(t->out_file, uid, PERM_WRITE) != 0) { ret = (uint32_t)-13; break; }
            int r = fs_write_raw_mode(t->out_file, b, (uint32_t)i, t->out_append ? 1 : 0);
            ret = r == 0 ? (uint32_t)i : (uint32_t)r;
            break;
        }
        vga_write(b); ret = (uint32_t)i; break;
    }
    case SYS_PUTC: vga_putc((char)a1); ret = 0; break;
    case SYS_CLS:  vga_clear(); ret = 0; break;
    case SYS_READLINE: {
        static char b[256];
        char *ub = (char *)a1; int max = (int)a2;
        if ((uint32_t)ub < USER_IMAGE_BASE || (uint32_t)ub >= USER_ARENA_END || max <= 0) { ret = (uint32_t)-3; break; }
        if (max > 255) max = 255;
        if (t->in_pipe >= 0) {
            /* v2.5: read one line from the pipe instead of the keyboard */
            pipe_t *pp = pipe_by_handle(t->in_pipe);
            if (!pp) { ret = (uint32_t)-1; break; }
            int bi = 0;
            for (;;) {
                while (pp->len == 0) {
                    if (!pp->open_w) break;          /* writer closed = EOF */
                    task_sleep_ticks(1);
                }
                if (pp->len == 0) break;
                char c = pp->buf[pp->head];
                pp->head = (uint16_t)((pp->head + 1) % PIPE_BUF);
                pp->len = (uint16_t)(pp->len - 1);
                b[bi++] = c;
                if (c == '\n' || bi >= max - 1) break;
            }
            b[bi] = 0;
            for (int j = 0; j < max; j++) { ub[j] = b[j]; if (!b[j]) break; }
            ret = 1;
            break;
        }
        while (!console_try_own()) task_block(T_WAIT_KB);
        kb_readline(b, max);
        console_release();
        for (int i = 0; ; i++) { char c = b[i]; if (i >= max - 1) c = 0; ub[i] = c; if (!c) break; }
        ret = 1;
        break;
    }
    case SYS_TIME_STR: {
        char *ub = (char *)a1;
        if ((uint32_t)ub < USER_IMAGE_BASE || (uint32_t)ub + 20 >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        ce_time_str_fill(ub);
        ret = 0; break;
    }
    case SYS_FS_READY: ret = fs_fs_ready(); break;
    case SYS_LS: {
        char p[FS_PATH_MAX];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        if (fs_perm_check(p, uid, PERM_READ) != 0) { ret = (uint32_t)-13; break; }
        /* snapshot the whole listing into this task's buffer */
        struct ls_fill lf; lf.ents = ls_buf[t->tid]; lf.n = 0;
        if (fs_foreach(p, ls_fill_cb, &lf) != 0) { ret = (uint32_t)-1; break; }
        t->ls_step = 0;
        t->ls_count = lf.n;             /* entry count for LS_NEXT */
        ret = 0;                           /* handle 0 (one ls at a time) */
        break;
    }
    case SYS_TOUCH: ret = (uint32_t)sys_touch_uid((const char *)a1, uid); break;
    case SYS_MKDIR: ret = (uint32_t)sys_mkdir_uid((const char *)a1, uid); break;
    case SYS_CAT: {
        char p[FS_PATH_MAX];
        static char b[4096];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        if ((uint32_t)a2 < USER_IMAGE_BASE || (uint32_t)a2 >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        int max = (int)a3; if (max > 4096) max = 4096;
        if (max <= 0) { ret = (uint32_t)-3; break; }
        if (fs_perm_check(p, uid, PERM_READ) != 0) { ret = (uint32_t)-13; break; }
        int n = fs_read_text(p, b, max);
        if (n > 0) for (int i = 0; i < n; i++) ((char *)a2)[i] = b[i];
        ret = (uint32_t)n;
        break;
    }
    case SYS_SAVE: ret = (uint32_t)sys_save_uid((const char *)a1, (const char *)a2, a3, uid); break;
    case SYS_ALLOC: ret = (uint32_t)sys_alloc(t, a1); break;
    case SYS_FREE:  sys_free(t, (void *)a1); ret = 0; break;
    case SYS_EXIT:  task_exit_current((int)a1);
    case SYS_PRINT_INT: wdec((int)a1); ret = 0; break;
    case SYS_PRINT_HEX: whex(a1); ret = 0; break;
    case SYS_SET_FG: { int c = (int)a1; if (c < 0) c = 0; if (c > 15) c = 15; vga_set_fg(c); ret = 0; break; }
    case SYS_DELAY: task_sleep_ticks(a1 / 10 + 1); ret = 0; break;
    case SYS_ATOI: {
        const char *s = (const char *)a1;
        if ((uint32_t)s < USER_IMAGE_BASE || (uint32_t)s >= USER_ARENA_END) { ret = 0; break; }
        int v = 0, neg = 0, i = 0;
        while (s[i] == ' ' || s[i] == '\t') i++;
        if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
        while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        ret = (uint32_t)(neg ? -v : v);
        break;
    }
    case SYS_STRLEN: {
        const char *s = (const char *)a1;
        if ((uint32_t)s < USER_IMAGE_BASE || (uint32_t)s >= USER_ARENA_END) { ret = 0; break; }
        uint32_t n = 0; while (s[n] && n < 4096) n++;
        ret = n; break;
    }
    case SYS_STRCMP: {
        const char *a = (const char *)a1, *b = (const char *)a2;
        if ((uint32_t)a < USER_IMAGE_BASE || (uint32_t)b < USER_IMAGE_BASE ||
            (uint32_t)a >= USER_ARENA_END || (uint32_t)b >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        uint32_t i = 0;
        while (a[i] && a[i] == b[i] && i < 4096) i++;
        ret = (uint32_t)((unsigned char)a[i] - (unsigned char)b[i]);
        break;
    }
    case SYS_STRCPY: {
        char *d = (char *)a1; const char *s = (const char *)a2;
        if ((uint32_t)d < USER_IMAGE_BASE || (uint32_t)s < USER_IMAGE_BASE ||
            (uint32_t)d >= USER_ARENA_END || (uint32_t)s >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        uint32_t i = 0;
        for (; s[i] && i < 256; i++) d[i] = s[i];
        d[i] = 0;
        ret = i;
        break;
    }
    case SYS_GETCH: {
        /* 阻塞等待键盘事件（不要求控制台所有权）；
           task_block 已让出 CPU，被键盘 IRQ 唤醒后直接重查队列。
           FIX(2026-10-27): 原代码 task_block 后 if(need_resched) reschedule()
           会把刚唤醒的任务又切走（need_resched 是键盘 ISR 设置的），
           在无抢占路径消费时导致 CE getch 永远读不到键。 */
        while (kb_peek() == 0) {
            task_block(T_WAIT_KB);
        }
        int k = kb_getkey();
        ret = (uint32_t)k;
        break;
    }
    case SYS_FILE_EXISTS: {
        char p[FS_PATH_MAX];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = 0; break; }
        uint32_t c, s;
        ret = fs_resolve_file(p, &c, &s) == 0 ? 1 : 0;
        break;
    }
    case SYS_FSIZE: {
        char p[FS_PATH_MAX];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-1; break; }
        uint32_t c, s;
        if (fs_resolve_file(p, &c, &s) != 0) { ret = (uint32_t)-1; break; }
        ret = s;
        break;
    }
    case SYS_NET_INIT: ret = net_init() < 0 ? (uint32_t)-1 : 0; break;
    case SYS_NET_UP:   ret = net_up(); break;
    case SYS_HTTP_GET: {
        char url[160];
        static char b[8192];
        if (u2k_str(url, (const char *)a1, sizeof(url)) != 0) { ret = (uint32_t)-1; break; }
        if ((uint32_t)a2 < USER_IMAGE_BASE || (uint32_t)a2 >= USER_ARENA_END) { ret = (uint32_t)-1; break; }
        int max = (int)a3; if (max > 8192) max = 8192;
        if (max <= 0) { ret = (uint32_t)-1; break; }
        int n = net_http_get(url, b, max);
        if (n > 0) for (int i = 0; i < n; i++) ((char *)a2)[i] = b[i];
        ret = (uint32_t)n;
        break;
    }
    case SYS_NET_DNS: {
        char host[128];
        if (u2k_str(host, (const char *)a1, sizeof(host)) != 0) { ret = (uint32_t)-1; break; }
        if ((uint32_t)a2 < USER_IMAGE_BASE || (uint32_t)a2 + 16 >= USER_ARENA_END) { ret = (uint32_t)-1; break; }
        uint8_t ip[4];
        if (net_dns_lookup(host, ip) != 0) { ret = (uint32_t)-1; break; }
        char *o = (char *)a2; int oi = 0;
        for (int i = 0; i < 4; i++) {
            int d = ip[i]; char tb[4]; int ti = 0;
            if (d == 0) tb[ti++] = '0';
            while (d) { tb[ti++] = (char)('0' + d % 10); d /= 10; }
            while (ti) o[oi++] = tb[--ti];
            if (i < 3) o[oi++] = '.';
        }
        o[oi] = 0;
        ret = 0;
        break;
    }
    case SYS_NET_UDP_SEND: {
        char ip[128];
        static char b[1400];
        if (u2k_str(ip, (const char *)a1, sizeof(ip)) != 0) { ret = (uint32_t)-1; break; }
        if ((uint32_t)a3 < USER_IMAGE_BASE || (uint32_t)a3 >= USER_ARENA_END ||
            (int)a4 <= 0 || (int)a4 > 1400) { ret = (uint32_t)-1; break; }
        for (int i = 0; i < (int)a4; i++) b[i] = ((const char *)a3)[i];
        ret = (uint32_t)net_udp_send(ip, (uint16_t)a2, (const uint8_t *)b, (uint32_t)a4);
        break;
    }
    case SYS_NET_UDP_RECV: {
        /* blocking receive with ~2 s timeout; polls the NIC while waiting
         * (also fixes the old "udp: no reply" bug - the RX path never
         * polled the card) */
        static char ipb[24]; static char b[2048];
        if ((uint32_t)a3 < USER_IMAGE_BASE || (uint32_t)a3 >= USER_ARENA_END) { ret = (uint32_t)-1; break; }
        int max = (int)a4; if (max > 2048) max = 2048; if (max <= 0) { ret = (uint32_t)-1; break; }
        int port = 0;
        uint32_t deadline = timer_ticks + 200;
        for (;;) {
            if (net_up()) net_poll();
            int n = net_udp_recv(ipb, &port, (uint8_t *)b, max);
            if (n > 0) {
                for (int i = 0; i < n; i++) ((char *)a3)[i] = b[i];
                if ((uint32_t)a1 >= USER_IMAGE_BASE && (uint32_t)a1 + 24 < USER_ARENA_END) {
                    int i;
                    for (i = 0; i < 23 && ipb[i]; i++) ((char *)a1)[i] = ipb[i];
                    ((char *)a1)[i] = 0;
                }
                if ((uint32_t)a2 >= USER_IMAGE_BASE && (uint32_t)a2 + 4 < USER_ARENA_END)
                    *(int *)a2 = port;
                ret = (uint32_t)n;
                break;
            }
            if ((int32_t)(timer_ticks - deadline) >= 0 || !tasks_on) { ret = 0; break; }
            task_sleep_ticks(1);
        }
        break;
    }
    case SYS_NET_PING: {
        char ip[128];
        if (u2k_str(ip, (const char *)a1, sizeof(ip)) != 0) { ret = (uint32_t)-1; break; }
        ret = (uint32_t)net_ping(ip);
        break;
    }
    case SYS_NET_IP: {
        char *o = (char *)a1;
        if ((uint32_t)o < USER_IMAGE_BASE || (uint32_t)o + 16 >= USER_ARENA_END) { ret = 0; break; }
        int oi = 0;
        for (int i = 0; i < 4; i++) {
            int d = net_our_ip[i]; char tb[4]; int ti = 0;
            if (d == 0) tb[ti++] = '0';
            while (d) { tb[ti++] = (char)('0' + d % 10); d /= 10; }
            while (ti) o[oi++] = tb[--ti];
            if (i < 3) o[oi++] = '.';
        }
        o[oi] = 0;
        ret = 0;
        break;
    }
    case SYS_GETUID: ret = (uint32_t)uid; break;
    case SYS_GETTID: ret = (uint32_t)t->tid; break;
    case SYS_KILL:  ret = (uint32_t)task_kill((int)a1, uid); break;
    case SYS_YIELD: task_yield(); ret = 0; break;
    case SYS_HW_NUM:
        ret = (uint32_t)hw_num((int)a1);
        break;
    case SYS_HW_STR: {
        char *ub = (char *)a2;
        int max = (int)a3;
        if ((uint32_t)ub < USER_IMAGE_BASE || (uint32_t)ub >= USER_ARENA_END ||
            max <= 0) { ret = (uint32_t)-1; break; }
        if (max > 4096) max = 4096;
        ret = (uint32_t)hw_str((int)a1, ub, max);
        break;
    }
    case SYS_LS_NEXT: {                    /* LS_NEXT(handle): one entry */
        int h = (int)a1;
        if (h != 0) { ret = (uint32_t)-1; break; }
        int idx = t->ls_step;
        if (idx >= t->ls_count) { ret = 0; break; }   /* done */
        ls_ent_t *ent = &ls_buf[t->tid][idx];
        t->ls_step = idx + 1;
        k2u_copy(&t->space, VS_LS_NAME, ent->name, 24);
        k2u_copy(&t->space, VS_LS_SIZE, &ent->size, 4);
        f->edx = ent->isdir ? 1 : 0;
        ret = 1;
        break;
    }
    /* ---- v2.5: big files (offset read / mode write / delete) ---- */
    case SYS_FILE_READ: {
        char p[FS_PATH_MAX];
        static char fbuf[16384];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        if ((uint32_t)a2 < USER_IMAGE_BASE || (uint32_t)a2 >= USER_ARENA_END) { ret = (uint32_t)-3; break; }
        int max = (int)a3; if (max > 16384) max = 16384; if (max <= 0) { ret = (uint32_t)-3; break; }
        int off = (int)a4; if (off < 0) off = 0;
        if (fs_perm_check(p, uid, PERM_READ) != 0) { ret = (uint32_t)-13; break; }
        int n = fs_read_at(p, fbuf, off, max);
        if (n < 0) { ret = (uint32_t)-1; break; }
        if (n > 0) for (int i = 0; i < n; i++) ((char *)a2)[i] = fbuf[i];
        ret = (uint32_t)n;
        break;
    }
    case SYS_FILE_WRITE: {
        char p[FS_PATH_MAX];
        int size = (int)a3, mode = (int)a4;
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        if (size < 0 || size > 16384) { ret = (uint32_t)-3; break; }
        if (mode != 0 && mode != 1) { ret = (uint32_t)-3; break; }
        if (size > 0 && ((uint32_t)a2 < USER_IMAGE_BASE || (uint32_t)a2 + size > USER_ARENA_END)) { ret = (uint32_t)-3; break; }
        if (fs_perm_check(p, uid, PERM_WRITE) != 0) { ret = (uint32_t)-13; break; }
        int r = fs_write_raw_mode(p, (const char *)a2, (uint32_t)size, mode);
        ret = r == 0 ? 0 : (uint32_t)r;
        break;
    }
    case SYS_FILE_DELETE: {
        char p[FS_PATH_MAX];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        if (fs_perm_check(p, uid, PERM_WRITE) != 0) { ret = (uint32_t)-13; break; }
        ret = (uint32_t)fs_delete(p);
        break;
    }
    /* ---- v2.5: pipes ---- */
    case SYS_PIPE_CREATE: {
        int idx = -1;
        for (int i = 0; i < PIPE_MAX; i++) if (!pipes[i].used) { idx = i; break; }
        if (idx < 0) { ret = (uint32_t)-1; break; }
        for (int k = 0; k < (int)sizeof(pipe_t); k++) ((uint8_t *)&pipes[idx])[k] = 0;
        pipes[idx].used = 1;
        pipes[idx].open_w = 1;
        pipes[idx].open_r = 1;
        ret = (uint32_t)(0x100 + idx);
        break;
    }
    case SYS_PIPE_WRITE: {
        pipe_t *pp = pipe_by_handle((int)a1);
        int len = (int)a3;
        if (!pp || len < 0 || len > PIPE_BUF) { ret = (uint32_t)-1; break; }
        if (len == 0) { ret = 0; break; }
        const char *ud = (const char *)a2;
        if ((uint32_t)ud < USER_IMAGE_BASE || (uint32_t)ud + len > USER_ARENA_END) { ret = (uint32_t)-3; break; }
        int wrote = 0, perr = 0;
        while (wrote < len) {
            if (!pp->open_r) { perr = -1; break; }       /* reader gone */
            int free = PIPE_BUF - 1 - pp->len;
            if (free <= 0) { task_sleep_ticks(1); continue; }
            int n = len - wrote; if (n > free) n = free;
            for (int k = 0; k < n; k++) {
                pp->buf[pp->tail] = ud[wrote + k];
                pp->tail = (uint16_t)((pp->tail + 1) % PIPE_BUF);
            }
            pp->len = (uint16_t)(pp->len + n);
            wrote += n;
        }
        ret = perr ? (uint32_t)perr : 0;
        break;
    }
    case SYS_PIPE_READ: {
        pipe_t *pp = pipe_by_handle((int)a1);
        int max = (int)a3;
        if (!pp || max < 0 || max > PIPE_BUF) { ret = (uint32_t)-1; break; }
        char *ub = (char *)a2;
        if ((uint32_t)ub < USER_IMAGE_BASE || (uint32_t)ub + max > USER_ARENA_END) { ret = (uint32_t)-3; break; }
        if (max == 0) { ret = 0; break; }
        int got = 0;
        for (;;) {
            while (pp->len == 0) {
                if (!pp->open_w) goto pipe_rd_done;    /* writer closed = EOF */
                task_sleep_ticks(1);
            }
            if (got >= max) break;
            ub[got++] = pp->buf[pp->head];
            pp->head = (uint16_t)((pp->head + 1) % PIPE_BUF);
            pp->len = (uint16_t)(pp->len - 1);
            if (pp->len == 0) break;    /* drained what we have: return now */
        }
    pipe_rd_done:
        ret = (uint32_t)got;
        break;
    }
    case SYS_PIPE_CLOSE: {
        pipe_t *pp = pipe_by_handle((int)a1);
        if (!pp) { ret = (uint32_t)-1; break; }
        if (t->out_pipe == (int)a1) { pp->open_w = 0; t->out_pipe = -1; }
        else if (t->in_pipe == (int)a1) { pp->open_r = 0; t->in_pipe = -1; }
        else { pp->open_w = 0; pp->open_r = 0; }
        if (!pp->open_w && !pp->open_r) { pp->used = 0; pp->len = 0; }
        ret = 0;
        break;
    }
    /* ---- v2.5: child processes with exit codes ---- */
    case SYS_SPAWN: {
        char p[FS_PATH_MAX];
        if (u2k_str(p, (const char *)a1, sizeof(p)) != 0) { ret = (uint32_t)-3; break; }
        int argc = (int)a2; uint32_t argv_u = (uint32_t)a3;
        if (argc < 0 || argc > CE_ARGC_MAX) { ret = (uint32_t)-3; break; }
        static char *kargv[CE_ARGC_MAX];
        static char kargv_buf[CE_ARGC_MAX][64];
        int bad = 0;
        for (int i = 0; i < argc; i++) {
            if (argv_u + (i + 1) * 4 > USER_ARENA_END) { bad = 1; break; }
            uint32_t aup = *(uint32_t *)(argv_u + i * 4);
            if (aup < USER_IMAGE_BASE || aup >= USER_ARENA_END) { bad = 1; break; }
            if (u2k_str(kargv_buf[i], (const char *)aup, 64) != 0) { bad = 1; break; }
            kargv[i] = kargv_buf[i];
        }
        if (bad) { ret = (uint32_t)-3; break; }
        ret = (uint32_t)task_spawn_ce(p, argc, kargv, uid);
        break;
    }
    case SYS_WAIT: {
        int tid = (int)a1;
        task_t *ch = task_by_tid(tid);
        if (!ch || ch->state == T_UNUSED) { ret = (uint32_t)-1; break; }
        task_wait_child(ch);
        ret = (uint32_t)ch->exit_code;
        break;
    }
    default: ret = (uint32_t)-8; break;
    }

    f->eax = ret;
    g_in_syscall--;
    (void)a5;
    /* cooperative preemption point: a long syscall gives up the CPU here
     * if the timer asked for a resched and someone else can take it */
    if (need_resched && tasks_on && !cur_task->ring0 && cur_task != &idle_task_v)
        preempt_at_syscall_exit(f);
}

/* ---- per-task stats for `top` -------------------------------------------- */

static uint32_t task_pages_of(task_t *t) {
    if (!t->space.pt) return 0;
    uint32_t n = 0;
    uint32_t *pt = (uint32_t *)t->space.pt;   /* kernel identity alias */
    for (uint32_t i = 0; i < 1024; i++)
        if (pt[i] & 1) n++;
    return n;
}

uint32_t task_idle_ticks(void) { return idle_ticks_v; }

int task_stat(int slot, int *tid, uint32_t *cpu_ticks, uint32_t *pages,
              int *state, int *uid, int *ring, char *name, int namelen) {
    if (slot < 0 || slot >= MAX_TASKS) return 0;
    task_t *t = &tasks[slot];
    if (t->state == T_UNUSED) return 0;
    if (tid) *tid = t->tid;
    if (cpu_ticks) *cpu_ticks = t->cpu_ticks;
    if (pages) *pages = task_pages_of(t);
    if (state) *state = t->state;
    if (uid) *uid = t->uid;
    if (ring) *ring = t->ring0 ? 0 : 3;
    if (name && namelen > 0) {
        int i;
        for (i = 0; t->name[i] && i < namelen - 1; i++) name[i] = t->name[i];
        name[i] = 0;
    }
    return 1;
}

/* ---- spawn ---------------------------------------------------------------- */

int task_spawn_ce(const char *path, int argc, char **argv, int uid) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_UNUSED) { slot = i; break; }
    if (slot < 0) return -1;

    uint32_t clus = 0, size = 0;
    if (!path || fs_resolve_file(path, &clus, &size) != 0) return -2;
    if (size < sizeof(ce_header_t) || size > CE_MAX_IMAGE + sizeof(ce_header_t)) return -3;

    /* v2.5: read the first cluster, parse the 20-byte header there (no
     * big bounce buffer).  The image then walks the FAT chain into the
     * already-mapped user pages below. */
    static uint8_t fbuf[FAT_CLUSTER_SZ_MAX];
    uint32_t csz = fat_cluster_size();
    if (fat_read_cluster(clus, fbuf) != 0) return -4;
    ce_header_t *h = (ce_header_t *)fbuf;
    if (h->magic[0]!=CE_MAGIC0||h->magic[1]!=CE_MAGIC1||h->magic[2]!=CE_MAGIC2||h->magic[3]!=CE_MAGIC3)
        return -5;
    uint32_t img = h->image_size, bss = h->bss_size, api = h->api_offset;
    if (h->entry != 0 || img > CE_MAX_IMAGE || bss > CE_MAX_BSS) return -5;
    if (img + sizeof(ce_header_t) > size) return -5;
    if (api + 4 > img) return -5;

    task_t *t = &tasks[slot];
    /* v2.5: always reset redirect fields -- tasks[] is static and a reused
     * slot keeps stale pipe/file state which pipe_task_cleanup (or the
     * redirect path) would then act on, corrupting the resume frame. */
    t->out_pipe = -1;
    t->in_pipe = -1;
    t->out_file[0] = 0;
    t->out_append = 0;
    uint8_t *ks = task_kstacks[slot - 1];
    uint32_t ks_top = (uint32_t)ks + sizeof(task_kstacks[0]);
    for (uint32_t i = 0; i < sizeof(task_kstacks[0]); i++) ks[i] = 0;

    if (task_space_create(&t->space) != 0) return -6;

    /* map image/bss pages (bss zero: kzalloc_frame) */
    uint32_t span = (img + bss + 0xFFF) & ~0xFFFu;
    for (uint32_t off = 0; off < span; off += 4096) {
        uint32_t f = (uint32_t)kzalloc_frame();
        if (!f) goto fail_space6;
        task_map_page(&t->space, USER_IMAGE_BASE + off, f, 1);
    }
    /* fill the image by walking the FAT chain directly into the mapped
     * pages; the header occupies the first 20 bytes of the first cluster,
     * so image bytes start at file offset 20 there and at 0 afterwards */
    {
        uint32_t c = clus, img_written = 0;
        while (c >= 2 && c < 0x0FFFFFF0 && img_written < img) {
            if (img_written != 0)
                if (fat_read_cluster(c, fbuf) != 0) goto fail_space4;
            uint32_t in_cluster = (img_written == 0) ? (csz > 20 ? csz - 20 : 0) : csz;
            uint32_t take = img - img_written; if (take > in_cluster) take = in_cluster;
            uint32_t src_off = (img_written == 0) ? 20 : 0;
            uint32_t va = USER_IMAGE_BASE + img_written;
            uint32_t k = 0;
            while (k < take) {
                uint32_t pa = task_virt_to_phys(&t->space, (va + k) & ~0xFFFu);
                uint32_t poff = (va + k) & 0xFFF;
                uint32_t n = 4096 - poff; if (n > take - k) n = take - k;
                uint8_t *d = (uint8_t *)(pa + poff);
                for (uint32_t x = 0; x < n; x++) d[x] = fbuf[src_off + k + x];
                k += n;
            }
            img_written += take;
            uint32_t n = fat_get(c);
            if (n == FAT_EOC || n < 2) break;
            c = n;
        }
        if (img_written < img) goto fail_space4;
    }

    /* vsyscall page (user read-only) */
    {
        uint32_t f = (uint32_t)kzalloc_frame();
        if (!f) goto fail_space6;
        task_map_page(&t->space, USER_VSYSCALL, f, 1);  /* RW: ls thunk scratch lives here */
        build_vsyscall_page((uint8_t *)f);
    }

    /* user stack (64 KiB) */
    for (uint32_t va = USER_STACK_TOP - USER_STACK_SZ; va < USER_STACK_TOP; va += 4096) {
        uint32_t f = (uint32_t)kzalloc_frame();
        if (!f) goto fail_space6;
        task_map_page(&t->space, va, f, 1);
    }

    /* argv strings + array near the stack top; entry frame below a dummy
     * return address pointing at the exit trampoline, so a plain
     * `return 0;` from ce_main exits the task cleanly */
    static uint32_t argv_ptr[CE_ARGC_MAX];
    int an = argc > CE_ARGC_MAX - 1 ? CE_ARGC_MAX - 1 : argc;
    uint32_t sp = USER_STACK_TOP - 0x10;
    for (int i = 0; i < an; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        uint32_t l = 0; while (s[l] && l < 64) l++;
        sp -= l + 1;
        k2u_copy(&t->space, sp, s, l + 1);
        argv_ptr[i] = sp;
    }
    uint32_t argv_arr = (sp - CE_ARGC_MAX * 4) & ~3u;
    for (int i = 0; i < an; i++) k2u_copy(&t->space, argv_arr + i * 4, &argv_ptr[i], 4);
    uint32_t usp = argv_arr - 12;
    uint32_t three[3] = { VS_EXIT_STUB, (uint32_t)an, argv_arr };
    k2u_copy(&t->space, usp, three, 12);
#ifdef SPAWN_DEBUG
    {
        uint32_t chk[3];
        uint32_t pa = task_virt_to_phys(&t->space, usp);
        chk[0] = *(uint32_t *)pa;
        chk[1] = *(uint32_t *)(pa + 4);
        chk[2] = *(uint32_t *)(pa + 8);
        w("[dbg usp="); whex(usp);
        w(" words="); whex(chk[0]); w(" "); whex(chk[1]); w(" "); whex(chk[2]);
        w(" vs0="); whex(*(uint32_t *)task_virt_to_phys(&t->space, USER_VSYSCALL));
        w(" vs300="); whex(*(uint32_t *)task_virt_to_phys(&t->space, USER_VSYSCALL + 0x300));
        w(" tbl0="); whex(*(uint32_t *)task_virt_to_phys(&t->space, VS_TABLE));
        w(" apislot="); whex(*(uint32_t *)task_virt_to_phys(&t->space, USER_IMAGE_BASE + api));
        w("]\n");
    }
#endif

    /* api table pointer into the image's api slot */
    {
        uint32_t pa = task_virt_to_phys(&t->space, USER_IMAGE_BASE + api);
        *(uint32_t *)pa = VS_TABLE;
    }

    /* hand-built ISR frame at the top of the kernel stack */
    int_frame_t *fr = (int_frame_t *)(ks_top - sizeof(int_frame_t) - 16);
    uint8_t *fb = (uint8_t *)fr;
    for (uint32_t i = 0; i < sizeof(int_frame_t); i++) fb[i] = 0;
    fr->eip = USER_IMAGE_BASE;             /* ce_main at image offset 0 */
    fr->cs = SEL_UCODE;
    fr->eflags = 0x202;                    /* IF set */
    fr->esp = usp;
    fr->ss = SEL_UDATA;
    fr->ds = fr->es = fr->fs = fr->gs = SEL_UDATA;

    t->tid = slot;
    t->uid = uid;
    t->ring0 = 0;
    t->state = T_READY;
    t->kstack_top = ks_top;
    t->ksp = (uint32_t)fr;
    t->resume_frame = 1;
    t->wake_tick = 0;
    t->exit_code = 0;
    t->parent = cur_task;
    t->heap_end = USER_HEAP_BASE;
    heap_free_root[slot] = 0;
    t->ls_count = 0;
    t->ls_step = 0;
    t->cpu_ticks = 0;
    {
        const char *nm = path, *slash = 0;
        while (*nm) { if (*nm == '/') slash = nm + 1; nm++; }
        const char *base = slash ? slash : path;
        int i;
        for (i = 0; base[i] && base[i] != '.' && i < 15; i++) t->name[i] = base[i];
        t->name[i] = 0;
    }
    return slot;

    /* F1 fix (v2.5.4): every failure after task_space_create used to
     * return without tearing the address space down, leaking the mapped
     * image/bss/vsyscall/stack frames plus the page-table frames. */
fail_space4:
    task_space_destroy(&t->space);
    return -4;
fail_space6:
    task_space_destroy(&t->space);
    return -6;
}
