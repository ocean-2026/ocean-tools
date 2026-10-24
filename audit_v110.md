# ot v1.10 增量功能/正确性审查报告（双审 · 第一环 · 只审功能）

- 审查对象：`ocean_tools/ocean_tools.c`（4355 行，镜像 65400 / 65536 B，余 136 B）
- 基线：v1.9.13（功能审 FINAL PASS），本次仅审 v1.10 增量
- 审查方式：Read/Grep 静态精读，未改动任何文件
- 日期：2026-10-24

---

## 0. 结论总评

# **FAIL**

发现 **1 个严重功能 bug（swapin 分片下标 off-by-one，直接破坏 swap 功能本体）** 与
**1 个状态泄漏 bug（g_call_depth 未在 csc_clear 复位，跨脚本运行可致野跳转）**。
其余 L3 帧、banner 外置、瘦身回归均通过。修复量很小（两处各一行/几行），修完即可转 PASS。

| 模块 | 结论 |
|---|---|
| L3 func/call/ret 帧 | PASS WITH ISSUES（帧逻辑本身正确，仅 csc_clear 漏复位 + 失败路径推帧） |
| swapin 分片绕行 | **FAIL（off-by-one，>512B 载荷重建错位/丢页）** |
| banner 外置 | PASS |
| 瘦身回归（help/about/kernelonly/edt） | PASS |
| CE 约束（尺寸/栈/bss/无浮点） | PASS |

---

## 1. 逐项核验表

### 1.1 L3 子程序（func/call/ret，行 302-307、715-861）

| 检查项 | 结果 | 证据 |
|---|---|---|
| call 时 split_args(t+k) 是否改当前行缓冲 | ✅ 无此问题 | `split_args` 形参为 `const char *args`（行 242），只拷贝进 argv，**不写回** t；且 call 后 pc 立即跳走，当前行不再执行 |
| 帧保存 $0..$9 完整 | ✅ | 行 831-832 保存 `i=0..CSC_MAXARGS`（含 argv[0]=脚本路径）；行 854-855 完整恢复 |
| $ret 走变量表 | ✅ | 行 849 `csc_set("ret", vx)`；"ret" 非数字名，csc_get 正确走变量表（行 321-332） |
| ret 弹帧回 call 行+1 | ✅ | 行 829 存 `pc`（call 行），行 856 `pc = g_call_ret[d]+1` |
| func 名预扫描注册标签 | ✅ | 行 729-741 把 `func NAME` 注册进 lbl 表；行 817 func 行本身 pc++ 跳过 |
| 深度 8 溢出保护 | ✅ | 行 827 `g_call_depth >= CALL_DEPTH(8)` 报错并 break |
| ret 无帧时行为 | ✅ 合理 | 行 857-859：仍 `csc_set("ret")`，然后 pc++ 顺序执行（顶层 ret 仅设变量并跳过该行） |
| **g_call_depth 跨脚本复位** | ❌ | `csc_clear()`（行 309-316）复位了 nvars/argc/fa/fb/for_depth/arrs，**唯独漏 g_call_depth**；脚本自然结束或 `call` 失败 break 后，残留深度带入下一脚本 |
| **call 失败路径推帧** | ⚠️ | 行 829-833 已 push 帧并自增 depth，行 838-840 找不到 func 才 break —— 失败也泄漏一帧 |
| func 名与 `:label` 冲突 | ⚠️ 轻微 | 预扫描不查重；查找循环命中**最先注册**者（行 812/838 break on first）。同名片时先出现者胜出，无报错 |
| 预扫描 nlbl 溢出 | ✅ | 行 722/734 `if (nlbl < CSC_MAXLABELS)` 保护，溢出静默丢弃（可接受降级） |

### 1.2 swapin 绕行（cmd_swapin，行 2965-2991）

| 检查项 | 结果 | 证据 |
|---|---|---|
| g_fa_len/g_fa_n 重置 | ✅ | 行 2975 `g_fa_len=0; g_fa_n=0;` |
| 主文件先 mergeb | ✅ | 行 2976 `csc_mergeb(SWAP_PATH)` |
| 分片路径 `.A`+两位数字拼接 | ✅ 拼接本身正确 | 行 2981-2983 两位十进制 |
| **分片下标起点** | ❌ **off-by-one** | 见 §2-BUG1 |
| g_swap_n 与实际分片数不一致 | ✅ 鲁棒 | 分片缺失即 break（行 2984），多陈旧分片被 `i<g_swap_n` 边界忽略 —— 这正是 v1.10 想要的语义，只是起点错了 |
| i<99 上限 | ✅ | 8192B→16 页→15 片，99 绰绰 |
| 重启后 g_swap_n=0 | ℹ️ 已知设计 | 行 2920 注释明示"RAM 页表，重启清空"；此时 swapin 只重建主文件(≤512B)且静默。可接受，建议输出提示 |

### 1.3 banner 外置（cmd_banner，行 2184-2219；scripts/BANNER.TXT）

| 检查项 | 结果 | 证据 |
|---|---|---|
| cat 失败降级纯文本 | ✅ | 行 2191 `if (n>0)` 否则走 2201-2207 纯标题 |
| csc_index_lines/fetch_row 边界 | ✅ | maxl=8（行 2192）；fetch_row 行 526 越界返空、行 531 截断到 outsz-1；ln[90] 足够（banner 行宽 ~60） |
| 行颜色逻辑 | ✅ | row1-5=标题色、row6=分隔线白；与 BANNER.TXT 实际 7 行（空/5 行美术/1 分隔线）吻合 |
| build.sh 宽度检查 | ✅ | 改读 scripts/BANNER.TXT，>80 列断言失败 |
| ⚠️ 副作用 | ℹ️ 轻微 | banner 复用 g_csc_fb/g_fb_off；脚本内 `run banner` 会覆盖 fb 态。banner 仅启动/about 调用，风险低 |

### 1.4 瘦身回归

| 检查项 | 结果 |
|---|---|
| help 覆盖关键命令 | ✅ 表 3814-3867 与 help 文本 2239-2256 一一对应，含 v1.9 命令补全（swapout/in/info、su/whoami/acl、clp/snap、cedbg、edt/fm、sft/vcs 等） |
| about 精简 | ✅ 行 2221-2232，语法无误，已提及 func/call/ret |
| kernelonly 文案 | ✅ 行 2767-2771 通用桩，reboot/hang/format 委托（3041-3043），无语法问题 |
| CE 约束 | ✅ 镜像 65400/65536；L3 帧在 bss（8×10×40=3200B）；无浮点；save/cat 路径未触碰 |

---

## 2. Bug 清单

### BUG-1（严重 / 功能失效）：swapin 分片下标 off-by-one

- **位置**：`ocean_tools.c:2977`
- **现象**：
  - 写入方约定：主文件 = page0；第一个尾片（bytes 512..1023 = page1）写入 **`.A00`**
    （多片走 `csc_save_shards` 行 497-500，idx 从 0 起；单片走行 2952 直接写 `.A00`）。
    即 page k（k≥1）↔ 分片 `.A0(k-1)`。
  - 旧 reader `csc_snapb`（行 604）循环 `sj = 0` → 读 `.A00` 起，**与写入方一致**。
  - v1.10 新 reader（行 2977）：`for (i = 1; i < g_swap_n; i++)` → 第一个就读 **`.A01`**，
    **永远跳过 `.A00`**。
- **后果**：任何 `swapout` 载荷 **>512B** 时，`swapin` 重建结果 =
  page0 ‖ page2 ‖ page3 …（page1 即字节 512-1023 整页丢失，后续全部下移 512B）。
  - 513-1024B 情形：只写了 main+`.A00`，reader 找 `.A01` 不存在 → break，**只回收到 512B**，尾部全丢。
  - swap 功能存在的意义就是突破 512B/8KB 限制 —— 该主用路径当前是坏的。
- **修复建议**（起点改为 0 基，片数 = g_swap_n-1）：
  ```c
  for (i = 0; i < (int)g_swap_n - 1 && i < 99; i++) {
      ... 拼接 ".A" + 两位 i ...
  }
  ```
  （即读 `.A00..A(g_swap_n-2)`，与 csc_save_shards/csc_snapb 约定对齐。）

### BUG-2（状态泄漏 / 可致野跳转）：g_call_depth 未在脚本启动复位

- **位置**：`csc_clear()` 行 309-316（缺复位）；配合行 840 call 失败也已推帧。
- **现象**：脚本非正常平衡返回（自然结束于 func 内、`call 不存在的名字` break、step-limit break）后，
  `g_call_depth` 残留 >0。下一次 `run xxx.csc` 只调 `csc_clear()`，**不清 depth**。
  下一脚本顶层一条 `ret` 会误判"有帧"：`g_call_depth--` 后恢复上轮残留的 argv，并把
  `pc = 残留 g_call_ret[d] + 1`（上轮脚本里的行号）—— 在新脚本里跳到错误行或越界退出。
- **修复建议**：
  1. `csc_clear()` 内加一行：`g_call_depth = 0;`（根治跨脚本泄漏）。
  2. （可选加固）call 失败查找失败时（行 840），在 break 前回滚帧：`g_call_depth--;`
     并恢复 `g_csc_argc/g_csc_argv`，避免单次运行内状态被半推帧污染。

### 附注（非阻塞）

- N1：`call` 实参不做 `$VAR` 展开（L3TEST.CSC 注释已声明为 v1.11 候选，镜像预算所限）—— 已知限制，非 bug。
- N2：`func NAME` 与 `:NAME` 同名不查重，先注册者胜出 —— 建议文档说明。
- N3：重启后 `g_swap_n=0`，swapin 静默只回收主文件 ≤512B —— 建议补一行提示 "page table lost after reboot"。
- N4：banner 复用 g_csc_fb；脚本内 `run banner` 会覆盖 fb —— 低风险，建议文档避免。

---

## 4. 复审段（commit 51394af，镜像 65400B）

针对上轮 BUG-1 / BUG-2 的修复复审：

### 4.1 BUG-1 修复核验：swapin 循环（行 2980 `for (i=0; i < (int)g_swap_n-1 && i<99; i++)`）

写入方约定复核（swapout 行 2937-2959 + csc_save_shards 行 483-507）：
- page0 = 主文件；page k(k≥1) ↔ 分片 `.A0(k-1)`；分片总数 = g_swap_n - 1。

逐边界推演：

| 载荷 n | g_swap_n | 写入方产物 | reader 循环行为 | 重建结果 | 结论 |
|---|---|---|---|---|---|
| ≤512B | 1 | 仅主文件（csc_save_shards len≤512 直存主文件，无分片） | `i < 1-1 = 0` 不进循环 | 主文件 = n | ✅ |
| 513..1024B | 2 | 主=page0 + `.A00`(尾片) | i=0：读 `.A00`；i=1 不满足 `1<1` 停 | 512+(n-512)=n | ✅ |
| 1025..1536B | 3 | 主=page0 + `.A00`(page1) + `.A01`(page2) | i=0 `.A00`、i=1 `.A01`；i=2 不满足 `2<2` 停 | 512+512+(n-1024)=n | ✅ |
| 一般 N 页 | N | `.A00..A(N-2)` 共 N-1 片 | i=0..N-2 正好读全 | 总字节 = n | ✅ |

附加边界：
- **重启后 g_swap_n=0**：`(int)0-1 = -1`，`i=0 < -1` 为假 → 不进循环，仅 mergeb 主文件，无 UB（int 比较），行为与上轮评审已知设计一致。✅
- **陈旧分片**：g_swap_n=2 但盘上残留旧 `.A01` —— 循环在 i=1 处因 `i<1` 边界自然停止，陈旧片被忽略，正是 v1.10 想要的语义。✅
- **缺片**：某中间分片缺失 → `file_exists` 假即 break，部分重建不崩溃。✅
- i<99 上限对 8KB 载荷（16 页/15 片）绰绰。✅

### 4.2 BUG-2 修复核验：csc_clear（行 315 新增 `g_call_depth = 0;`）

- csc_clear 在 run_script_args（行 694）每个脚本启动时必调 → 跨脚本残留帧彻底清零。✅
- 复位顺序正确：csc_clear() 先于 `g_csc_argc=ac` / `argv[0]=path`（行 695-696），新脚本从 depth=0、空帧栈起步。✅
- 上轮提到的"call 失败路径已 push 帧才 break"（行 ~840）：因失败即 break 出脚本、下一次 csc_clear 已强制清零，单次运行内不再有后续执行，半推帧不再有实际危害，**可选加固不再需要**。✅
- func/call/ret 正常流程未被触碰，L3TEST.CSC 语义不受影响。✅

### 4.3 复审结论

# **PASS（修复确认）**

BUG-1、BUG-2 均按建议方向修复，边界 512/513/1024/1025 及重启/陈旧片/缺片全部推演正确。
上轮 4 条非阻塞附注（N1-N4）维持原状，不阻塞交付。v1.10 功能审第一环至此 **PASS**。
建议（非阻塞）上真机跑一轮：swapout ~2KB 文件 → swapin → 字节比对；以及 L3TEST.CSC 后紧跟一个含顶层 ret 的脚本做端到端确认。
