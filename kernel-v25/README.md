# CatOS 内核 v2.5 一次性改造 · 源码快照（2026-10-28/29）

> 基线：`Catos_Better_better`（用户 2026-10-27 上传 zip，md5 a07a19d77fd723f7b587e901ca494e17）。
> 本目录仅含**本次改动/新增**的核心源码与测试程序，供审阅与复用；完整工程在
> `catos-qemu/Catos_Better_better/Catos_Better_better/`（含 62MB 镜像/磁盘，未入 git）。
> 构建：`sh build.sh iso`（Linux GNU 工具链，需 `-fleading-underscore`，见 history.md）。

## 改动清单（对应 CatOS-内核问题清单.md #37/38/42/43/44/45）

| 文件 | 改动 |
|---|---|
| src/task.c | SYS_FILE_READ(39)/WRITE(40)/DELETE(41)、SYS_PIPE_CREATE(42)/WRITE(43)/READ(44)/CLOSE(45)、SYS_SPAWN(46)/WAIT(47)；**resume_frame 帧恢复修复**（isr_syscall 入口清 + schedule_to park 分支清 prev）；task_spawn_ce 重定向字段显式初始化；SYS_WRITE 三级重定向（pipe→file→vga）；CE_MAX 提升后 FAT 链直写映射页 |
| src/fs.c | fs_read_at（offset 簇定位）、fs_write_raw_mode（mode0 截断/mode1 追加）、fs_delete、fs_perm_check 位掩码 PERM_READ/WRITE/EXEC |
| src/fat.c | fat_append_file（链尾续写）、fat_flush（已知性能缺陷：每次写 8MB 全区） |
| src/shell.c | parse_redir/spawn_ce_redirect/cmd_run：shell 重定向 `>` `>>` `|` |
| src/kernel.c | CE_MAX_IMAGE 64KB→256KB、CE_MAX_BSS 256KB→512KB |
| src/ce.c | 大镜像加载适配（移除 ce_bounce 大缓冲） |
| include/ce_abi.h | catos_api_t 48 槽（v2.5 尾部九槽） |
| build.sh / boot/boot.s / extapp/build_ce.sh | Linux GNU 构建链（-fleading-underscore、nm 探测 entry、python 算 16 进制） |
| extapp/test_ab.c | 全功能回归：getuid/大文件写读/offset 读/管道/spawn+wait 退出码/file_delete |

## QEMU 实测结论（2026-10-29 复测，全通过）

- test_ab：`TEST v2.5 ALL PASS`（含 child exit=0、100KB 写、offset 读、delete）
- shell：`run hello.ce > /tmp/h.txt`（输出消失→0B）、`>>`（→100B）、`run gen_ce.ce | /bin/sink_ce.ce`（SINK got 11 lines）
- GP 崩溃（spawn+wait 后 pop gs）已修复；pipe_read 死锁已修复
- 详细：见 ../problem.md、../CatOS-内核问题清单.md

## 已知遗留
- fat_flush 每次写 8MB FAT 全区（性能，功能正常，后续脏段追踪）
- #46 POSIX syscall 入口面（libc 层）未做——下一步用户侧 libc 封装
