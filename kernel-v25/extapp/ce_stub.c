/*
 * ce_stub.c -- freestanding runtime stubs for CE programs.
 *
 * build_ce.sh links this next to every app.  MinGW gcc emits a call to
 * __chkstk_ms before allocating a stack frame larger than one page
 * (128 KB of local arrays in cc.c, for example).  CatOS CE tasks run on
 * a known 8 KiB kernel-side stack that the loader has already mapped,
 * so probing pages is pointless -- the probe must simply be a no-op.
 *
 * Anything else libgcc would supply (division helpers on i386 are native
 * instructions, so usually nothing) can be added here.
 */

/* Signature used by the MS/sysv i386 ABI: called with the frame size in
 * EAX and the current stack pointer in ECX; it must not clobber a caller
 * register beyond what a normal function may.  An empty body satisfies
 * the linker and the ABI contract. */
void __chkstk_ms(void) {
    __asm__ volatile("" ::: "memory");
}
