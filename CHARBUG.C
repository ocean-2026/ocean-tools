/* CHARBUG.C - FIND-S1-01 regression: a char struct member indexed with [i]
 * must store ONE byte (88 08), not a 4-byte mov (89 08) which would clobber
 * the adjacent int field x.  Layout: buf[4] at off 0, x at off 4 (aligned).
 * Writing buf[1] after setting x=42 must leave x untouched.
 * Compiled ON CatOS by cc.ce.  Expected output:
 *   x=42 */
typedef struct {
    char buf[4];
    int x;
} rec_t;

int ce_main(int argc, char **argv) {
    rec_t r;
    r.buf[0] = 1;
    r.buf[1] = 2;
    r.buf[2] = 3;
    r.buf[3] = 4;
    r.x = 42;
    /* A 4-byte store here would write bytes 1..4 and zero the low byte of x. */
    r.buf[1] = 9;
    r.buf[2] = 8;
    r.buf[3] = 7;
    api->write("x="); api->print_int(r.x);
    api->write("\n");
    return 0;
}
