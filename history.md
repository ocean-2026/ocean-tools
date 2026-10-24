# history.md - 易错点记录

格式：[所属项目][易错点]内容
判定准则：只要是因为管理不当或非技术原因导致的、细心就能避免的意外，就算易错点（2026-10-24 用户定稿）。

- [ot][易错点] execve 加载 CE 时 image 必须**跳过 16 字节 CE01 头**从 fb+16 拷入 0x02000000；若连头一起拷，0x02000000 处是 'CE01' 魔数数据，ce_main 实际在 +16 处，call 0x02000000 执行垃圾指令导致输出乱码（v1.9 乱码根因，已修）
- [ot][易错点] QEMU 新启动实例的 PS/2 键盘注入可能**完全失效**（HMP sendkey / QMP input-send-event / 长 hold 均 0% 成功率，实测 12+ 新实例全丢），仅长时间运行的老实例部分正常；VM 交互测试前先发单键探测键盘，不可假设键盘可用
- [ot][易错点] `pkill -f qemu-system` 会匹配 bash -c 包装进程的命令行导致**误杀自身 shell**（exit 137）；杀 QEMU 用 pidfile 或 `ps aux | grep "[q]emu" | awk '{print $2}' | xargs -r kill -9`
- [ot][易错点] exec_launch 用 i386 固定寄存器约束（S/D/c/a/d/b 共 6 个）触发 GCC "asm operand has impossible constraints"（32 位仅 8 个寄存器）；参数改走栈（"m" 内存操作数 push + push 立即数 + ret 跳转）
- [ot][易错点] QEMU monitor 是行式交互，新连接会先收到欢迎横幅（"type 'help' for more information"），脚本解析响应时勿把横幅当命令回显
- [ot][易错点] vga.py 的 q_cmd 超时 1s 会截断 xp 大输出（2000 字 dump 需 ~10s），monitor 命令积压拖垮后续 sendkey；读屏超时应 ≥10s
- [ot][易错点] v1.9 exec 跳板需完全位置无关（拷贝到固定区 0x0147c800 执行），跳板内引用外部符号地址（如 g_csc_fb）会导致跳板内容含绝对地址失效；用寄存器传递/自我计算替代
- [CatOS双线开发][易错点] 多项目并行共享同一 QEMU 环境时，用 `pkill -f qemu` / `pkill qemu` 全量清理会**连坐杀掉另一项目正在运行的 VM**（ot 链周期清理时游戏链 VM 被误杀 3 次）；正确做法：① 各项目用独立磁盘副本（adofai/disk-test.img）+ 独立 monitor socket（catos-mon.sock vs catos-mon2.sock）；② 清理只按自身 pidfile 精确 kill 或按 socket 文件归属匹配，禁止全杀；③ 共用 disk.img 时改盘前先确认对方未在用
- [ot][易错点] C 函数参数名遮蔽全局函数：my_readline(char *out, ...) 的参数 out 遮蔽了全局 out() 输出函数，函数体内调用 out() 报 "called object 'out' is not a function or function pointer"；参数名避免与全局函数重名，或改用 outln/out_int/直接 api->write0
- [ot][易错点] QEMU sendkey 长 hold 参数（sendkey o 100）会触发按键重复风暴，对已处于 #34 键盘失效的实例产生大量乱码注入（屏幕出现 0x13a14511 类内存 dump 乱码），实例即废；键盘失效时勿再用 hold 强化重试
