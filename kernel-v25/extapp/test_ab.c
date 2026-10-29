/* test_ab.c - v2.5 kernel-side feature test:
 *   getuid / big file write+offset read / pipe / spawn+wait / file_delete
 * Build: sh extapp/build_ce.sh test_ab.c  -> extapp/bin/test_ab.ce
 * Run:   run /bin/test_ab.ce   (copy into /bin first)
 */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    char b[512];
    int i, n;
    (void)argc; (void)argv;

    a->write("TEST v2.5 start\n");

    /* 1. uid */
    a->print_int(a->getuid());
    a->write(" = uid\n");

    /* 2. big file write: 100 KiB in 16 KiB chunks (truncate then append).
     * 16 KiB keeps the per-call FAT flush cost down (the kernel currently
     * flushes both FATs on every write). */
    {
        char chunk[16384];
        for (i = 0; i < 16384; i++) chunk[i] = (char)('a' + (i % 26));
        int parts[7] = { 16384, 16384, 16384, 16384, 16384, 16384, 4096 };
        for (int w = 0; w < 7; w++) {
            if (a->file_write("/tmp/big.txt", chunk, parts[w], w == 0 ? 0 : 1) != 0) {
                a->write("file_write FAIL\n"); return 1;
            }
        }
        int sz = a->fsize("/tmp/big.txt");
        if (sz != 100 * 1024) {
            a->write("big size FAIL ");
            a->print_int(sz);
            a->write("\n");
            return 2;
        }
        a->write("big write 100K OK\n");
    }

    /* 3. offset read check: each 16 KiB chunk is the same 'a'..'z' pattern,
     * so byte at offset 64*512 (== chunk[0]) must be 'a' */
    {
        char rb[16];
        n = a->file_read("/tmp/big.txt", rb, 16, 64 * 512);
        if (n != 16) { a->write("offset read FAIL\n"); return 3; }
        if (rb[0] != 'a') {
            a->write("offset data FAIL ");
            a->print_int(rb[0]);
            a->write(" != 97\n");
            return 4;
        }
        a->write("offset read OK\n");
    }

    /* 4. pipe self test (create / write / read / close) */
    {
        int h = a->pipe_create();
        if (h < 0) { a->write("pipe create FAIL\n"); return 5; }
        const char *msg = "PIPE-OK";
        if (a->pipe_write(h, msg, 7) != 0) { a->write("pipe write FAIL\n"); return 6; }
        char pb[16];
        n = a->pipe_read(h, pb, 16);
        if (n != 7) { a->write("pipe read FAIL\n"); return 7; }
        for (i = 0; i < 7; i++)
            if (pb[i] != msg[i]) { a->write("pipe data FAIL\n"); return 8; }
        a->pipe_close(h);
        a->write("pipe OK\n");
    }

    /* 5. spawn + wait child exit code (hello.ce prints and returns 0) */
    {
        char *av[2];
        av[0] = "/bin/hello.ce";
        int tid = a->spawn(av[0], 1, av);
        if (tid <= 0) { a->write("spawn FAIL\n"); return 9; }
        int code = a->wait(tid);
        a->write("child exit=");
        a->print_int(code);
        a->write("\n");
    }

    /* 6. file_delete */
    {
        if (a->file_delete("/tmp/big.txt") != 0) { a->write("delete FAIL\n"); return 10; }
        if (a->file_exists("/tmp/big.txt") != 0) { a->write("delete check FAIL\n"); return 11; }
        a->write("delete OK\n");
    }

    a->write("TEST v2.5 ALL PASS\n");
    return 0;
}
