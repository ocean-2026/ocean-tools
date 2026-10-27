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

## Windows 适配（2026-10-26/27）

- ot[易错点] msys64 mingw32 的 ld 只支持 i386pe、不支持 elf_i386：CE 构建必须走 PE 路线 —— `ld -m i386pe -T <ce.ld> -e _ce_main` + `objcopy -O binary -R .reloc`（排除 PE 重定位段）+ nm/objdump 取 bss/api + mkce.py 装 CE01 头。参照 Catos_Better_better/extapp/build_ce.sh。
- ot[易错点] mingw gcc 对大栈帧函数自动插入 __chkstk_ms 栈探测，freestanding CE 环境无此符号：编译加 `-fno-stack-check -mno-stack-arg-probe`。
- ot[易错点] mingw 下 C 符号加下划线前缀：`ce_main`→`_ce_main`（ld -e 用 `_ce_main`），`__ce_api`→`___ce_api`（static 符号，nm 显示小写类型 `d`，awk 匹配 `[dDbBrR]`）。
- ot[易错点] QEMU 在 Windows 不支持 -monitor unix socket：改用 `-monitor tcp:127.0.0.1:4444,server,nowait`，python socket 发 screendump/sendkey。
- ot[易错点] PowerShell 5.1 下 `2>&1 | Out-File`/`| Out-String` 偶发吞掉输出：验证脚本结果写文件再读（cmd /c "type" 最稳）。
- catos[易错点] **调度器禁止从 ISR 切换任务**：CatOS 的 `schedule_to()` 注释明确 "never call from an ISR"。曾在 `console_wake_all()`（被键盘 IRQ 调用）里直接 `reschedule()`，导致 ring3 CE 任务（demo/ot）的 getch 永远读不到键（ISR 内切换破坏中断帧）；shell 是 ring0 合作切换侥幸未炸。修复：ISR 只置 `need_resched=1`，由 syscall 返回前 `preempt_at_syscall_exit` 或 idle_main 循环消费。
- catos[经验] 排查"CE 收不到键"类问题：先用 `top` 看任务 STATE（T_WAIT_KB = 正确阻塞在等键 → 问题在唤醒/调度；非阻塞乱跑 → 问题在应用层）；再用"前后台对照"排除键盘队列竞争（后台 demo + shell 同时等键会抢键，属设计行为）。
- ot[经验] PE 路线 CE 构建全流程（Windows 无 ELF 工具链时的替代）：gcc -m32 -Os -ffreestanding ... -nostdlib → ld -m i386pe -e _ce_main（ce_main 需 `.text.start` 段属性放镜像 offset 0）→ nm 校验 ce_main @ 02000000 → objcopy -O binary -R .reloc → objdump .bss size + nm __ce_api 地址减 CE_LOAD_BASE → mkce.py 打包。产物 63,044B（≤64KB 上限）。
- ot[经验] FAT32 注入无需工具：python 按 MBR/DBR/FAT 几何读写（复用 Catos_Better_better/build/inject_bin.py 逻辑），vhdx 用 qemu-img convert 转 raw 注入后再转回。
