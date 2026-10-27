# ot 项目问题记录（problem.md）

> 更新：2026-10-26（Windows 本机，新内核 Catos_Better_better 2026-10-26 高危修复版）
> 范围：ot 适配新内核过程中发现/验证的问题

## P1（内核回归，阻塞 ot 交互验证）

**[CatOS 新内核] CE getch（API 22）收不到键盘事件**
- 现象：新内核（Catos_Better_better，2026-10-26 高危缺陷修复版）下，所有 CE 应用的 getch 都拿不到键：
  - 内置 demo.ce 运行到 "press any key to exit ..." 后按任意键无反应，永不退出；
  - ot（/BIN/OCNTOOLS.CE）启动画面、FAT 挂载、网络、RTC 全部正常，但 REPL 输入（help/ls/任意字符）无响应，提示符光标持续闪烁（my_readline 阻塞在 api->getch()）。
- 对照（证明是内核问题而非 ot 问题）：
  - 内核 shell 键盘正常：catos:/$ ls 等命令可输入、可执行、可回车；
  - CE 加载/运行正常：demo 输出全部 API 结果（int/hex/strcpy/atoi/file_exists/fsize/delay），ot 完整显示欢迎界面；
  - 仅 CE 层的 getch 无事件。
- 影响：ot（及一切依赖 getch 的 CE 应用）无法交互；ot 的自动化测试（VERIFY.CSC/L3TEST.CSC）依赖键盘恢复后才能跑。
- 处置：已记录并上报；等待 Catos_Better_better 内核侧修复 CE 键盘事件转发（嫌疑点：ring3 CE 任务的键盘中断唤醒/缓冲转发在修复轮中回归）。修复前 ot 在 VM 内只能验证"启动+输出"，无法验证交互。

## P2（工具链，已解决）

**[Windows 构建] MinGW ld 不支持 ELF（ld -m elf_i386 失败）**
- 现象：msys64 mingw32 的 ld 只支持 i386pe，ot 原 build.sh（ELF 路线）在 Windows 直接失败。
- 解决：参照 Catos_Better_better/extapp/build_ce.sh 的 PE 路线 —— ld -m i386pe -T ot_ce_pe.ld -e _ce_main + objcopy -O binary -R .reloc + nm/objdump 取 bss/api + mkce_win.py 装 CE01 头。ot 源码 ce_main 已有 .text.start 段属性，无需改码。

## P3（工具链，已解决）

**[Windows 构建] __chkstk_ms 未定义**
- 现象：mingw gcc 对大栈帧自动插入 __chkstk_ms 栈探测，freestanding 环境无此符号。
- 解决：编译加 -fno-stack-check -mno-stack-arg-probe。

## P4（工具链，已解决）

**[Windows 构建] static 符号在 nm 中为小写类型**
- 现象：ot 的 __ce_api 是 static（nm 显示 d ___ce_api，三下划线前缀），原 awk 匹配 [DR] 大写类型取不到。
- 解决：awk 匹配 [dDbBrR]，符号名匹配 _*_?ce_api$。

## P5（验证方法）

**[QEMU] Windows 下 -monitor unix socket 不可用**
- 解决：改用 -monitor tcp:127.0.0.1:4444,server,nowait，python socket 发送 screendump/sendkey 控制。
- 备注：PowerShell 5.1 中 2>&1 | Out-File 管道偶发吞输出，验证脚本输出改走文件最稳。
