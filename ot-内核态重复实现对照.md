# ot 用户态实现 vs 内核态能力对照（2026-10-27 云端，Catos_Better_better 基线）

> 目标：按用户要求"把新内核已有的内核态实现、而我们又用户态实现了的功能恰当去除（如分页文件等）"。
> 基线：**Catos_Better_better**（用户 2026-10-27 上传 zip，D:\CatOs\Catos_Better_better，含 getch 修复版内核 + disk.vhdx）。
> 内核源码：`Catos_Better_better/Catos_Better_better/`（src/ + include/ce.h，PE/ELF 双路线可构建）。
> 状态：**已完成一轮去除（ACL），QEMU 实测通过**。

## 结论（基于 Catos_Better_better 实测）

**真正重复项 = ot 用户态合作 ACL（su/whoami/acl）→ 已去除**。其余均非重复，保留。

| ot 用户态功能 | 新内核（Catos_Better_better）是否已有 | 判定 |
|---|---|---|
| **su/whoami/acl（合作 ACL）** | **有：内核态 uid 权限系统**（task.c 544-620：perm_tab/fs_perm_set/fs_perm_check 强制；SYS_SAVE/SYS_TOUCH/SYS_MKDIR 均走写权限检查，SYS_SAVE 失败返 -13；fs_perm_seed 预设 /home /tmp /bin /appdevelop /etc） | **重复 → 已去除**（v1.10 的"合作非强制"版被内核强制版取代；ot 内置 PERMS.CE 亦可演示） |
| **swap（swapout/swapin，/TMP/SWAP.DAT 512B 分片）** | **无**：内核只有内存分页（paging.c：frame allocator + 页目录，非磁盘 swap）；CE API 无 swap；**FS_DATA_MAX=512 未变**（include/fs.h），cat 内核实为 4096B 硬截（task.c:808，ot 侧缓冲 8KB） | **非重复 → 保留**（用户点名"分页文件"：实测内核无磁盘分页文件，swap 是加载 >8KB CE 镜像的唯一通道） |
| 文件命令 dir/ls/cd/mkdir/touch/cat/files | 内核 FAT32 API（fs_ready/ls/touch/mkdir/cat/save/file_exists/fsize）+ **ls 带回调**（ce_ls_cb：is_dir/name/size/ud） | ot 已直接用回调式 ls（`api->ls(rp, db_cb, &c)`），命令层封装，**非实现重复** |
| 网络命令 ping/dns/curl/udp/netinfo | 内核 8 网络 API（net_init/up/http_get/dns/udp_send/udp_recv/ping/ip） | 命令层封装，**非重复** |
| reboot/hang/format/netdiag | 内核内置命令（CE 无 API） | ot 用 cmd_kernelonly 显式转发提示——**已按"内核态已有则不重复"原则处理** |
| edit/edt/fm/mem/cecheck/cedbg | 内核 shell 有 edit（ring0），但 CE 无 API 可调（cmd_kernelonly 已证）；其余内核无 | ot 版是 CE 环境唯一入口，**保留** |
| csc/run（CSC 2.0 + L3 func/call/ret） | 内核无脚本引擎 | 非重复 |
| vcs/sft（版本控制） | 内核无；ot 内已外置为外部 .csc 脚本（v1.6/1.7 起） | 非重复 |
| clp/snap/modal（剪贴板/快照/对话框） | 内核无 | 非重复 |
| resolve_existing 路径解析 | 内核 tty.c"扩展软件库自动发现(PATH)"是卷发现层，语义不同 | CE 内解析必要，**保留** |
| exec/run（用户态 CE 加载器跳板） | 内核有 task_spawn_ce（shell 用），**但 CE API 无 spawn/run 暴露**（ce.h 无此槽位） | ot 绕行仍必要，**保留** |

## 已执行的去除（2026-10-27）

**ot 合作 ACL 移除**（ocean_tools.c）：
- 删除命令：su / whoami / acl（TABLE 3 条目 + cmd_su/cmd_whoami/cmd_acl 实现）
- 删除机制：g_acl_pass/g_acl_uo/g_acl_admin/g_acl_no/g_acl_prot 数据 + acl_cpy/acl_load/acl_admin_only/acl_protected 函数 + ACL_PATH
- 删除调用点：cmd_cat 的 acl_protected 保护检查（2 处）+ run_line 命令分发的 acl_admin_only 检查（2 处）
- help 更新：去掉 "su/whoami/acl permissions"，modal 独立行
- 体积：65,432B → **63,832B**（-1,600B）
- **QEMU 实测**（新内核 catos.iso + 注入 ot 的磁盘）：ot 启动正常、help 无 ACL 命令、ls 17 项正常、键盘交互全通

## 保留项的判据（防止误删）

1. **swap**：FS_DATA_MAX=512、cat 内核实为 4096B 硬截（task.c:808）、无 delete/append/随机访问 —— 新内核 fs.h 原样保留这些限制；swap 是唯一能加载 >8KB CE 镜像的通道。若未来内核提供大文件 API/磁盘 swap，再移除。
2. **edit/edt/fm**：CE 无调用内核 shell 编辑器的 API（cmd_kernelonly 机制证明），ot 版为 CE 环境唯一编辑器。
3. **ls 回调**：ot 已直接用，无重复实现。

## 遗留建议

- 内核态权限系统（perm 表）目前仅强制写检查（SYS_SAVE/SYS_TOUCH/SYS_MKDIR）；读权限内核未查，ot 的 `cat` 可读任何文件。如需读保护，属内核功能需求 → **已写入 CatOS-内核问题清单.md #37，不擅自修内核**。
- 若希望 ot 的权限信息与内核 uid 对齐（如 whoami 显示内核 uid），需要内核暴露 uid 查询 API —— 同样记入清单待内核侧支持。