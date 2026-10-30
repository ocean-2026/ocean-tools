/* spawn_fail.c - F1 regression: task_spawn_ce failure path must not leak
 * the child address space. 40 failed spawns (bogus path) must not exhaust
 * page frames: afterwards a real spawn of hello.ce must still succeed.
 * Build: sh extapp/build_ce.sh spawn_fail.c -> extapp/bin/spawn_fail.ce
 * Run:   run /bin/spawn_fail.ce
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    (void)argc; (void)argv;
    int i, fails = 0;
    char *av[2];

    a->write("SPAWNF test start\n");

    /* 40 failed spawns of a nonexistent path: each must return <= 0.
     * If the failure path leaked the address space, the pool would be
     * exhausted long before 40 and later spawns would fail. */
    for (i = 0; i < 40; i++) {
        av[0] = "/NOPE/DOES_NOT_EXIST.ce";
        int tid = a->spawn(av[0], 1, av);
        if (tid > 0) {
            a->write("unexpected spawn success at iter ");
            a->print_int(i);
            a->write("\n");
            a->wait(tid);
            return 1;
        }
        fails++;
    }
    a->write("40 failed spawns OK (all tid<=0)\n");

    /* now a real spawn must still work (memory intact) */
    av[0] = "/bin/hello.ce";
    int tid = a->spawn(av[0], 1, av);
    if (tid <= 0) {
        a->write("real spawn FAIL after 40 failures\n");
        return 2;
    }
    int code = a->wait(tid);
    a->write("real spawn exit=");
    a->print_int(code);
    a->write("\n");
    if (code != 0) { a->write("SPAWNF FAIL\n"); return 3; }
    a->write("SPAWNF ALL PASS (no leak)\n");
    return 0;
}
