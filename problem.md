# ot 项目问题记录（problem.md）

> 更新：2026-10-27（Windows 本机，新内核 Catos_Better_better + getch 修复版）
> 范围：ot 适配新内核过程中发现/验证的问题

## P1（内核回归，**已修复** 2026-10-27）

**[CatOS 新内核] CE getch（API 22）收不到键盘事件**
- 现象：新内核（Catos_Better_better，2026-10-26 高危缺陷修复版）下，**所有 CE 应用的 getch 都拿不到键**：
  - 内置 `demo.ce` 运行到 "press any key to exit ..." 后按任意键无反应，永不退出；
  - ot（/BIN/OCNTOOLS.CE）启动画面、FAT 挂载、网络、RTC 全部正常，但 REPL 输入（help/ls/任意字符）无响应，提示符光标持续闪烁（my_readline 阻塞在 api->getch()）。
- 对照（证明是内核问题而非 ot 问题）：
  - 内核 shell 键盘正常：`catos:/$ ls` 等命令可输入、可执行、可回车；
  - CE 加载/运行正常：demo 输出全部 API 结果（int/hex/strcpy/atoi/file_exists/fsize/delay），ot 完整显示欢迎界面；
  - 仅 CE 层的 getch 无事件。
- **根因（2026-10-27 定位）**：`src/task.c` 的 `console_wake_all()` 在**键盘 ISR（kb_irq）里调用 `reschedule()` → `schedule_to()`**——而 `schedule_to` 注释明确 "never call from an ISR"：ISR 内切换任务会破坏中断帧，ring3 CE 任务被置 T_READY 后无法正确恢复，getch 永远读不到键。shell（ring0 任务）走合作切换路径侥幸未炸，掩盖了该 bug。另 SYS_GETCH 的 while 循环在 task_block 后又 `if(need_resched) reschedule()`，把刚唤醒的任务又切走（need_resched 是 ISR 设置的），进一步加剧。
- **修复**（Catos_Better_better/src/task.c，2 处）：
  1. `console_wake_all()` 删除 ISR 内 reschedule，只置 `need_resched=1`（改由安全路径消费：syscall 返回前 `preempt_at_syscall_exit` / idle_main 循环 pick_ready）；
  2. SYS_GETCH 的 while 循环删除多余的 `if(need_resched) reschedule()`（task_block 已让出 CPU，恢复后直接重查队列）。
- **验证（QEMU 实测，2026-10-27）**：修复后 —— demo.ce 前台运行按任意键正常退出（"bye from demo!" → 回 shell）；ot（OCNTOOLS.CE）REPL 键盘完全打通：help 输出完整命令表、ls 列出 17 项目录、外置 CSC 扫描正常。
- 内核重编译产物：kernel.bin 5,083,144B / catos.iso 15,810,560B（已复制到 D:\CatOs\ot\catos\）。

## P2（工具链，已解决）

**[Windows 构建] MinGW ld 不支持 ELF（ld -m elf_i386 失败）**
- 现象：msys64 mingw32 的 ld 只支持 i386pe，ot 原 build.sh（ELF 路线）在 Windows 直接失败。
- 解决：参照 Catos_Better_better/extapp/build_ce.sh 的 PE 路线 —— `ld -m i386pe -T ot_ce_pe.ld -e _ce_main` + `objcopy -O binary -R .reloc` + nm/objdump 取 bss/api + mkce_win.py 装 CE01 头。ot 源码 ce_main 已有 `.text.start` 段属性，无需改码。
- 易错点已按规则记入 history.md。

## P3（工具链，已解决）

**[Windows 构建] __chkstk_ms 未定义**
- 现象：mingw gcc 对大栈帧自动插入 __chkstk_ms 栈探测，freestanding 环境无此符号。
- 解决：编译加 `-fno-stack-check -mno-stack-arg-probe`。

## P4（工具链，已解决）

**[Windows 构建] static 符号在 nm 中为小写类型**
- 现象：ot 的 __ce_api 是 static（nm 显示 `d ___ce_api`，三下划线前缀），原 awk 匹配 `[DR]` 大写类型取不到。
- 解决：awk 匹配 `[dDbBrR]`，符号名匹配 `_*_?ce_api$`。

## P5（验证方法）

**[QEMU] Windows 下 -monitor unix socket 不可用**
- 解决：改用 `-monitor tcp:127.0.0.1:4444,server,nowait`，python socket 发送 screendump/sendkey 控制。
- 备注：PowerShell 5.1 中 `2>&1 | Out-File` 管道偶发吞输出，验证脚本输出改走文件最稳。
