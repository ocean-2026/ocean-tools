/* SWITCH.C - stage 2 regression: switch/case/default.
 * Expected output: c1=100 c2=200 c5=500 c9=-1 */
int classify(int n) {
    int r;
    r = 0;
    switch (n) {
    case 1: r = 100; break;
    case 2: r = 200; break;
    case 5: r = 500; break;
    default: r = -1; break;
    }
    return r;
}

int ce_main(int argc, char **argv) {
    api->write("c1="); api->print_int(classify(1));
    api->write(" c2="); api->print_int(classify(2));
    api->write(" c5="); api->print_int(classify(5));
    api->write(" c9="); api->print_int(classify(9));
    api->write("\n");
    return 0;
}
