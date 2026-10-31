/* GOTO.C - stage 3 regression: goto/label and prefix/postfix ++/--.
 * Expected output: s=10 i=5 post=3 i2=4 pre=5 */
int ce_main(int argc, char **argv) {
    int i, s;
    i = 0; s = 0;
loop:
    if (i >= 5) goto done;
    s = s + i;
    i++;
    goto loop;
done:
    api->write("s="); api->print_int(s);
    api->write(" i="); api->print_int(i);
    i = 3;
    api->write(" post="); api->print_int(i++);
    api->write(" i2="); api->print_int(i);
    api->write(" pre="); api->print_int(++i);
    api->write("\n");
    return 0;
}
