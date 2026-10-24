# history.md - 易错点记录

格式：[所属项目][易错点]内容

- [ot][易错点] execve 加载 CE 时 image 必须**跳过 16 字节 CE01 头**从 fb+16 拷入 0x02000000；若连头一起拷，0x02000000 处是 'CE01' 魔数数据，ce_main 实际在 +16 处，call 0x02000000 执行垃圾指令导致输出乱码（v1.9 乱码根因，已修）
- [ot][易错点] QEMU 新启动实例的 PS/2 键盘注入可能**完全失效**（HMP sendkey / QMP input-send-event / 长 hold 均 0% 成功率，实测 12+ 新实例全丢），仅长时间运行的老实例部分正常；VM 交互测试前先发单键探测键盘，不可假设键盘可用
- [ot][易错点] `pkill -f qemu-system` 会匹配 bash -c 包装进程的命令行导致**误杀自身 shell**（exit 137）；杀 QEMU 用 pidfile 或 `ps aux | grep "[q]emu" | awk '{print $2}' | xargs -r kill -9`
- [ot][易错点] exec_launch 用 i386 固定寄存器约束（S/D/c/a/d/b 共 6 个）触发 GCC "asm operand has impossible constraints"（32 位仅 8 个寄存器）；参数改走栈（"m" 内存操作数 push + push 立即数 + ret 跳转）
- [ot][易错点] QEMU monitor 是行式交互，新连接会先收到欢迎横幅（"type 'help' for more information"），脚本解析响应时勿把横幅当命令回显
- [ot][易错点] vga.py 的 q_cmd 超时 1s 会截断 xp 大输出（2000 字 dump 需 ~10s），monitor 命令积压拖垮后续 sendkey；读屏超时应 ≥10s
- [ot][易错点] v1.9 exec 跳板需完全位置无关（拷贝到固定区 0x0147c800 执行），跳板内引用外部符号地址（如 g_csc_fb）会导致跳板内容含绝对地址失效；用寄存器传递/自我计算替代
