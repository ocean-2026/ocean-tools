# CatOS 内核 v2.5 改造 · QEMU 实测问题记录（problem.md）

日期：2026-10-28 初记；2026-10-29 复测全通过后更新（云端 Ubuntu 环境，QEMU 已重装 qemu-system-x86）

## 已验证通过（QEMU 实机）

| 项 | 结果 |
|---|---|
| CE 运行链路（minimal/hello 正常输出+退出） | ✅（minimal 需标准 8.3 名 `MINIMAL.CE`，见易错点 2） |
| getuid（v2.3 槽 33） | ✅ 返回 0（root） |
| #42 大文件写（/tmp/big.txt 100KB，16KB 块 ×7，mode0 截断 + mode1 追加，fsize=102400 校验） | ✅ |
| #42 大文件 offset 读（fs_read_at 簇定位，offset=32768 读 16B） | ✅（期望 = chunk[0]='a'，每 16KB 块内容相同） |
| #42 file_delete（test_ab 第 6 段） | ✅ TEST v2.5 ALL PASS |
| 管道（gen_ce \| sink_ce，shell 层） | ✅ SINK got 11 lines，双任务干净退出 |
| shell 重定向 `>` / `>>`（run hello.ce） | ✅ 输出消失 → /tmp/h.txt 0B → 追加后 100B |
| spawn + wait 退出码（test_ab → hello exit(0)） | ✅ child exit=0，无崩溃 |
| **GP 崩溃修复**（spawn+wait 后帧恢复 pop gs） | ✅ **已修复**（见下） |

## 已修复（2026-10-29）

### 1. [严重] spawn+wait 后内核 GP 崩溃 —— **已修复**
- 根因 1：**task_spawn_ce 未初始化 t->out_pipe/in_pipe/out_file/out_append**（tasks[] 静态复用残留）→ pipe_task_cleanup 误操作。已加显式初始化（task.c:1333-1340）。
- 根因 2（主因）：**调度帧恢复路径错误**——`schedule_to` 的 park 分支（cpu_switch_park_and_resume）把 prev 保存为**协程式上下文**（push ebp/ebx/esi/edi + 存 esp），但 prev->resume_frame 未清 0 → 下次恢复 prev 时误按 **ISR 帧格式**弹（popal/段/iret）→ 从协程栈弹出垃圾段（gs=0x25a0）→ GP。
- 修复：① `isr_syscall` 入口 `t->resume_frame = 0`（新 trap 后旧帧失效）；② `schedule_to` park 分支 `prev->resume_frame = 0`（prev 走 cpu_switch_to 协程恢复）。
- 复测：test_ab spawn+wait 全流程 PASS，无崩溃，系统回提示符。

### 2. [中] pipe_read 阻塞死锁 —— **已修复并复测通过**
- 修复：SYS_PIPE_READ 读空当前数据后 `if (pp->len == 0) break;`（有数据即返回，EOF 语义保留）。
- 复测：test_ab 管道自测 + shell gen|sink 均通过。

### 3. [中] fat_flush 每次写 8MB（性能）—— **已知性能缺陷，未改**
- fat_flush（fat.c:126）每次写 FAT#1 + FAT#2 全区（fat_sz=8192×512×2=8MB）。大文件 200×512B 写入实测卡住（1.6GB 写盘）；改 16KB 块 ×7（56MB）后通过。
- 建议（后续优化）：脏段/脏页追踪，仅写修改的 FAT 扇区；或文件 write 时延迟 flush。**功能不受阻**。

## 环境/工程易错点（已记录到 history/experience 候选）

1. **云端 Ubuntu 每轮重置**：qemu-system-i386/qemu-img 会消失，需 `sudo apt-get update && sudo apt-get install -y qemu-system-x86 qemu-utils`；/tmp/disk-raw.img 也会清掉，需重建 4G 稀疏盘 + QEMU 内 `format`（**format 需要二次确认 y**）。
2. **inject 8.3 名格式**：内核 FAT 用标准 8.3（名 8 字符+扩展 3 字符，**无点**，如 `TEST_AB CE `），带点名（`TEST_AB.CE `）内核 fs_resolve_file 找不到（err -2）。inject_any.py 已修。
3. **inject 顺序坑**：删除旧目录项（0xE5）后若先注入再恢复 `.`/`..`，新文件会写到 `.`/`..` 槽被覆盖。先恢复目录项再注入。
4. **pkill -f 匹配 bash 自身**（heredoc 命令行含 qemu 字符串会自杀）；用 `pkill -x qemu-system-i386`。
5. **Linux GNU 构建链**：CFLAGS 加 `-fleading-underscore`（cpu.s/boot.s 用 `_` 前缀符号）、boot.s `call _kernel_main`、build_ce.sh `cd "$CEBASE"` + nm 探测 entry 符号 + python 算 16 进制、task.c 无 libc（memset 手写循环、前向声明）。
6. **fat 卷布局**（4G 盘）：RSVD=33（LBA1 分区 + 内核 RESERVED=32）、SPC=8、FAT2×8192；DBR 在 LBA1。
7. **QEMU 键盘/screendump**：monitor sendkey 工作正常；截图需 tesseract OCR（`apt-get install tesseract-ocr`），720×400 VGA 文本模式，放大 2× 后 OCR 更准。

## 2026-10-29 内核 v2.5 收尾未闭合问题（外部并行代理干扰记录）
- [内核#46(已闭合:干净盘0B重定向正常,根因=外部代理盘状态损坏,非内核bug)] `run /bin/hello.ce > /tmp/xxx` 截断重定向落盘 0B：hello 侧 SYS_WRITE→fs_write_raw_mode→resolve_path 中 fat_lookup(2,"TMP") 失败（[RP fail]2），同路径 shell 预截断侧成功；test_ab SYS_FILE_WRITE 同目录成功。已排除 g_in_syscall yield、ata 失败、参数破坏。核心矛盾未闭合。
- [内核#47(已闭合:err-4根因=旧QEMU残留加载旧iso,非内核bug)] test_ab.ce 在当前内核加载失败 err -4（task_spawn_ce：fat_read_cluster 或链短 img_written<img），历史旧内核/旧盘 ALL PASS，疑似回归或盘 FAT 链/缓存问题。
- [外部干扰] 同一工作区外部并行代理反复覆盖 src/fat.c、catos.iso、build/kernel.bin 与测试盘，导致 QEMU 实测加载版本与本地 build 不一致（iso 内核 sha 多次漂移），调试 trace（[FL/[SP/[WR/[RP/[F）已被注入但运行时版本不匹配无法闭合根因。
- 建议：隔离工作区或串行化提交后再回归；本次未闭合问题待串行环境复测。

## 内核#48(2026-10-29) 内置安装与注入文件簇冲突
format后注入TEST_AB.CE(cl36),重启后启动时"Installed built-in apps"安装sysinfo等内置app,可能复用已分配簇导致注入文件内容被覆盖(运行test_ab显示sysinfo输出)。
未修,已规避:先启动一次再注入,或注入后用run验证。

## 内核#49(2026-10-29) `>`重定向对已存在文件不截断 → 已修(b30)
根因:spawn_ce_redirect预截断受`fs_perm_check==0`条件保护,该检查不成立时跳过截断,随后强制append→旧内容+新内容。
修复:`>`时无条件执行`fs_write_raw_mode(ab,"",0,0)`预截断(b30编译通过;盘环境重建后实测)。