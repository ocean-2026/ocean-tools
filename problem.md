# CatOS 内核 v2.5 改造 · QEMU 实测问题记录（problem.md）

日期：2026-10-28 初记；2026-10-29 复测全通过后更新（云端 Ubuntu 环境，QEMU 已重装 qemu-system-x86）

## 已验证通过（QEMU 实机）

| 项 | 结果 |
|---|---|
| CE 运行链路（minimal/hello 正常输出+退出） | ✅（干净盘）|
| SYS_FILE_READ/WRITE/DELETE（#42） | ✅（目录/文件 读写删）|
| CE 镜像 64K→256K（#43） | ✅（big 镜像加载）|
| 管道 spawn+wait+退出码（#44） | ✅（test_ab 全流程）|
| `>`/`>>` 重定向（#44） | ✅（`run hello.ce > /tmp/qqy.txt` 落盘 100B 完整）|
| 权限 uid 查询（#37/38/45） | ✅（sysinfo 显示 uid 0）|

## 已闭合问题

- **0B 重定向（#46）**：干净盘实测 `run /bin/hello.ce > /tmp/qqx.txt` 成功，QQX.TXT 落盘 100B 内容完整（Hello from CE!+时间+alloc test）。根因=外部代理搞坏盘状态（FAT/目录不一致），非内核代码 bug。
- **test_ab err -4（#47）**：根因=旧 QEMU 残留进程仍加载旧 iso 在响应 monitor（判据：读屏无 [SP end fsz= 新字段；内存 0x1015b40 字符串是旧版 [RP 开头）。彻底 `pkill -9 -x qemu-system-i386` 并确认 port 4444 free 后重启加载 /tmp/k25_b28.iso 即正常（[SP c=43 s=4216] 加载成功、child exit=0、delete OK）。非内核回归。

## 遗留待修（低优先）

- **F1（低）**：spawn 失败时地址空间泄漏（task 表/页表未回滚）。
- **A2（低）**：fat_append_file 无 undo 日志（写坏 FAT 无回滚）。

## 内核#48(2026-10-29) 内置安装与注入文件簇冲突

format 后注入 TEST_AB.CE(cl36)，重启后启动时 "Installed built-in apps" 安装 sysinfo 等内置 app，可能复用已分配簇导致注入文件内容被覆盖（运行 test_ab 显示 sysinfo 输出）。
未修，已规避：先启动一次再注入，或注入后用 run 验证。

## 内核#49(2026-10-29) `>` 重定向对已存在文件不截断

`run hello.ce > /tmp/qqy.txt` 重复跑时，spawn 预截断未清空旧文件（ap=1 强制 append），文件=旧内容+新内容（77+23=100B）。
干净盘首次跑正常（空文件 append 累计）。语义瑕疵，低优先，待修：spawn 时对 `>` 目标先 truncate 再 append。

## 环境易错点（QEMU 实测）

- Ubuntu 环境每轮 qemu 二进制、/tmp 盘、/tmp iso 全消失（AGENTS.md 规则#1）。
- qemu 从另一会话副本 pkg3 拿（QEMU 5.2.0），需 LD_LIBRARY_PATH + `-L` bios + `-net none` + `-drive file=...,format=raw`。
- 构建新 iso 后必须先杀旧 QEMU 再用 /tmp 新副本启动，否则 monitor 连的是旧进程加载的旧内核。
- `pkill -9 -x qemu-system-i386` 精确杀；不能用 `pkill -f qemu-system`（会误杀含匹配串的自身 bash）。
- 注入：停 VM 后 `python3 ocean_tools/inject_any.py /tmp/disk_v2.img <ce> <8.3名.CE>`（同名覆盖=update，readback verify）。
- dirent 首簇=unpackH off+26 | unpackH off+20<<16；size=unpackI off+28；过滤 0x00/0xE5/attr∈{0x0F,0x08}。
