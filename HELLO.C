/* HELLO.C - source compiled ON CatOS by cc.ce (target one bootstrap test).
 * No #include allowed in the cc subset: api-> calls are built in. */
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int sum(int n) {
    int i;
    int t;
    t = 0;
    i = 1;
    while (i <= n) {
        t = t + i;
        i = i + 1;
    }
    return t;
}

int ce_main(int argc, char **argv) {
    api->write("cc-compiled hello\n");
    api->write("fib(10)=");
    api->print_int(fib(10));
    api->write(" sum(5)=");
    api->print_int(sum(5));
    api->write("\n");
    return 0;
}
