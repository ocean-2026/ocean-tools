# ot（ocean_tools）UI/UX 审美审查报告（双审闭环第二环）

- 审查对象：`ocean_tools/ocean_tools.c`（产物 `ocntools.ce`）
- 审查性质：只读审美环（功能审已 PASS）。未修改任何源码/产物/镜像。
- 证据来源：源码行号 + 截图 t3_ot_boot_rgb / t4_ot_help_rgb / t5_ot_ls_rgb；对照 extapp/hello.c、sysinfo.c、src/shell.c、busybox/Unix 惯例。

## 维度 1：help 信息架构 / 对齐 — 3/5（小修后已改善）

- 硬编码空格两栏表，无分组小标题；`netinfo/netdiag`（15 字符）、`swapout/in/info`（14 字符）左说明偏右 3 列，`reboot/hang/format/netdiag raw`（32 字符）顶破命令列。
- **已落地修复（v1.10.1）**：命令列统一 18 列定宽重排（ocean_tools.c:2117-2131），超长命令独占整行，t4 截图两栏全对齐。

## 维度 2：视觉一致性 — 4/5（小修后已改善）

- 主题色集中于 :1594-1596（title=9 浅蓝 / tagline=14 黄 / prompt=11 浅青），可被 OCNTOOLS.CFG 覆盖（:1617-1625）；错误统一浅红 12；ls 语义色 [DIR]浅青/[FILE]浅绿（:1736-1742）。
- 扣分点（小修前）：浅青 11 一身三职（提示符+help 正文+目录标记）、黄 14 同时是标语+help 标题+exit 行。
- **已落地修复（v1.10.1）**：help 正文 11→15（白），exit 行 14→13（品红），浅青让还给提示符与 [DIR]。

## 维度 3：反馈与状态 — 4/5（小修后已改善）

- 提示符 `render_prompt`（:3770-3780）：`ocean`(浅青):`cwd`(浅蓝)`$`，cwd 感知（sh/bash 式，优于 extapp/shell 裸提示符）；错误前缀统一 `prog: message` + 浅红。
- 扣分点（小修前）：三张截图顶部均残留幽灵 `ocean:$`（cwd 未初始化就打了提示符）。
- **已落地修复（v1.10.1）**：cwd 初始化提前至 ce_main 开头（cfg_load 前），Boot time 改局部缓冲 tb[32]（不再复用 cwd），t3/t4/t5 顶部已无幽灵行。

## 维度 4：密度与冗余 — 3/5

- 无超 80 列行（最长横幅 78 个 `=`）；netdiag 在 :2124 与 :2131 重复出现（双路径设计，非阻断）；启动幽灵提示符已消除。

## 维度 5：与同类对比定位 — 4/5

- 明显高于 extapp 四应用（hello.c 纯白文、sysinfo.c 单色手填标签），整体超过内核 shell 单色水准；主题化/彩色提示符/路径感知反超 busybox，但 help 分组（无小标题）仍落后 busybox 的定宽分组。

## Top 3 改进（已全部落地，v1.10.1，commit 52c47aa）

1. **cmd_help 定宽排版**：命令列 18 列统一，超长命令独占行，错位消除（:2117-2131 重排）。
2. **幽灵提示符根除**：cwd 初始化提前至 ce_main 开头，Boot time 独立缓冲（:4162 后 + 局部 tb[32]）。
3. **颜色分层**：help 正文 11→15、exit 14→13 品红（:2116/2132）。

## 总评

**PASS(小修)**——配色体系统一、反馈规范明显优于内置 CE 应用；小修（help 对齐/幽灵提示符/颜色分层）已在 v1.10.1 全部落地并经 QEMU 实测（t3 无幽灵行、t4 help 全对齐、t5 ls 17 项 [DIR] 青色正常）。不阻断交付。