/* STRUCT.C - stage 1 regression: struct / typedef / enum / . / -> / sizeof.
 * Compiled ON CatOS by cc.ce.  Expected output:
 *   x=7 y=10 gp=17 nm=hi A=0 B=1 C=10 D=11 sz=20 */
typedef struct {
    int x;
    char name[12];
    int y;
} point_t;

enum { A, B, C = 10, D };

point_t gp;

int ce_main(int argc, char **argv) {
    point_t p;
    point_t *pp;
    p.x = 5;
    p.y = p.x * 2;
    p.name[0] = 'h';
    p.name[1] = 'i';
    p.name[2] = 0;
    pp = &p;
    pp->x = 7;
    gp.x = p.x + pp->y;
    api->write("x="); api->print_int(p.x);
    api->write(" y="); api->print_int(p.y);
    api->write(" gp="); api->print_int(gp.x);
    api->write(" nm="); api->write(p.name);
    api->write(" A="); api->print_int(A);
    api->write(" B="); api->print_int(B);
    api->write(" C="); api->print_int(C);
    api->write(" D="); api->print_int(D);
    api->write(" sz="); api->print_int(sizeof(point_t));
    api->write("\n");
    return 0;
}
