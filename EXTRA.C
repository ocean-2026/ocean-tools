/* EXTRA.C - cc.ce P1 修复回归用例（由修复后的 cc.ce 在 CatOS 内编译运行）。
 * 覆盖：FIND-05 移位操作数、FIND-04 for+continue、FIND-04b 无 step 的 for、
 *       FIND-02 全局指针串 \0 转义、FIND-03 全局 char 数组串初始化。
 * 注意：cc 子集不支持后缀 ++/--，step 一律用 i = i + 1 等价形式。
 * 预期输出行：sh=12 t=8 c=3 z=12 msg=hi p1=A p2=B */
char g_msg[8] = "hi";     /* FIND-03: 数组串初始化 -> 内联字节 "hi\0" */
char *g_p = "A\0B";       /* FIND-02: 指针串初始化 -> 解码 'A',0,'B',0 */

int twice(int n) {
    return n * 2;
}

int ce_main(int argc, char **argv) {
    int sh, i, t, c, z;
    sh = 3 << 2;              /* FIND-05: 必须为 12（旧版错得 24） */
    t = 0;
    for (i = 0; i < 5; i = i + 1) { /* FIND-04: continue 必须执行 step */
        if (i == 2) continue;
        t = t + i;            /* 0+1+3+4 = 8 */
    }
    c = 0;
    for (i = 0; i < 3;) {     /* FIND-04b: 无 step，条件必须每次重评估 */
        c = c + 1;
        i = i + 1;
    }
    z = twice(6);             /* 12 */
    api->write("sh="); api->print_int(sh);
    api->write(" t="); api->print_int(t);
    api->write(" c="); api->print_int(c);
    api->write(" z="); api->print_int(z);
    api->write(" msg="); api->write(g_msg);
    api->write(" p1="); api->write(g_p);
    api->write(" p2="); api->write(g_p + 2);
    api->write("\n");
    return 0;
}
