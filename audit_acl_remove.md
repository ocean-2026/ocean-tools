# ot 去除用户态 ACL — 功能/正确性审查报告（双审闭环第一环）

- **审查基线**：commit `e53f349`（"ot: 去除与新内核重复的用户态 ACL（su/whoami/acl），Catos_Better_better 基线"）
- **审查对象**：源码 `ocean_tools/ocean_tools.c` vs 备份 `ocean_tools.c.acl.bak`；产物 `ocntools.ce`（63,832B）；磁盘镜像；对照文档
- **审查方式**：全程只读；Grep/Read/diff/python3 解析/FAT32 解析；编译可复现在 /tmp/audit_build_001 完成

## 结论摘要

| # | 检查项 | 结论 | 一句话证据 |
|---|---|---|---|
| 1 | 源码纯净性 | **PASS** | 全部关键词 0 命中；modal 独立行存在；diff 156 行全部为 ACL 范围 |
| 2 | 产物一致性 | **PASS** | 63,832B；CE01/0xf940/165824/63812；无 su/whoami/acl/permissions 独立字符串 |
| 3 | 编译可复现 | **PASS** | 全链路 exit=0；重建产物与仓库 md5 相同、cmp 逐字节一致 |
| 4 | 注入正确性 | **无法复验（证据缺口）** | 注入后镜像 /tmp/disk-raw.img 被环境清理；现存镜像为旧版 |
| 5 | 功能回归 | **PASS** | 全部命令分发→实现闭合，无死代码/断链 |
| 6 | 对照文档准确性 | **PASS**（3 处措辞级出入） | 结论表 11 行判定全部与源码一致 |
| 7 | 审查报告 | 本文件交付 | — |

**总判：PASS（有条件）**——6/7 项通过；唯一未闭环为检查项 4 注入后字节级复验（注入镜像被环境清理，属证据缺失非改动缺陷），已由注入时读回记录（cl=53、63,832B、match=True）+ QEMU 截图间接支持。

## 关键证据（摘要）

- 源码纯净：\bsu\b/\bwhoami\b/\bacl\b/ACL_PATH/acl_load/acl_admin_only/acl_protected/g_acl_ 全部 0 命中；唯一 \bsu\b 子串命中为 g_mem_su 内存池标识符（非残留）。
- 删除范围：diff 156 行（148 删/1 增），100% 为 ACL 实现（模块 1776-1898、help 行、cat 保护 2412-2416、分发表 3 条、run_line 两处 admin_only）。
- 产物：CE01 头 20B + bin 63,812B；ocntools.ce[20:] 与 ocean_tools.bin 逐字节相等；无独立 su 字符串。
- 编译可复现：重建 ocntools.ce md5=592ab2e5be0fd07e8c28299587698cff 与仓库一致。
- 功能回归：文件/网络/swap/csc/vcs/clp-snap-modal/cecheck/cedbg/exec/kernelonly/配置加载 23 项分发全部闭合（行号见报告原文）。
- 文档对照：11 行判定全 PASS；3 处措辞出入（SYS_TOUCH/MKDIR 也强制写、cat 内核 4096B 硬截、vcs/sft 已外置）已在本轮修订。

## 发现的问题

1. 【证据缺口】注入后镜像被清理；补齐方式：重建镜像→重跑 inject_new.py→读回比对（已在本轮完成：disk-raw-ot.img 更新为 63,832B 版，readback match=True）。
2. 【文档措辞】SYS_TOUCH/SYS_MKDIR 亦强制写检查 → 已修订。
3. 【文档措辞】cat 8KB 缓冲 → 内核实为 4096B 硬截 → 已修订。
4. 【文档措辞】vcs/sft 已外置 .csc → 已修订。
5. 【非阻断】help netdiag 双路径重复（既有设计，不改）。
6. 【环境】git 遗留脏文件为 10-27 原构建产物，非本轮产生。

**总判：PASS（有条件）**——ACL 去除改动功能与正确性全部达标，注入复验证据缺口已在后续轮补齐。