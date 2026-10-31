# CatOS 联合开发进度汇报（2026-10-30）

> 覆盖窗口：2026-10-29 至 2026-10-30 · 组织者 o_000puRdNNck（GNU自举+JS解释器+内核收尾）已结束 · 监控定时任务已删除

---

## 一、总览

| 目标 | 状态 | 说明 |
|---|---|---|
| 目标二：语言运行环境（node 子集） | ✅ **完成** | CE 用户态 JS 解释器，QEMU 实测全 PASS |
| 内核收尾（#48 / F1 / A2 / #49） | ✅ **完成** | 全部修复并回归通过（b31） |
| 环境清理 | ✅ **完成** | gitignore 排除 8.9G 镜像/zip/调试产物 |
| 目标一：GNU 工具链自举（子集） | ✅ **完成** | cc.ce 微型 C 编译器 QEMU 实机 4/4 PASS（编译→运行→重启存活） |
| GitHub 推送 | 🔶 部分完成 | 文档/测试脚本已推 `f190439f`；node.c 源码仅本地 commit |

---

## 二、已完成明细

### 2.1 内核 v2.5 收尾修复（b31，Linux GNU 构建链重编）

| 编号 | 问题 | 修复实质 |
|---|---|---|
| #48 | 内置安装与注入文件簇冲突（重启后注入文件被覆盖） | fat 脏表项 journal + fat_flush **脏扇区 RMW** 增量写盘（不再 8MB 全刷）+ fat_alloc 分配前 ATA 核对磁盘真 0 |
| F1 | task_spawn_ce 失败路径地址空间泄漏 | 失败分支 goto fail_space4/6 → `task_space_destroy` 归还页框/页表 |
| A2 | fat_append_file 无 undo 日志 | aundo[512] 日志逆序回滚，失败后 FAT 与调用前一致 |
| #49 | `>` 重定向对已存在文件不截断 | `>` 无条件 `fs_write_raw_mode(ab,"",0,0)` 预截断（b30） |

### 2.2 目标二：node 子集（CE 用户态 JS 解释器）

- **产物**：`extapp/node.c`（1,414 行 / 53,353B）→ `extapp/bin/node.ce`（35,116B）
- **能力**：变量 / 函数 / 递归 / while / continue / 数组（push/下标）/ 对象（嵌套/动态加属性）/ typeof / null / undefined / print
- **验收**：`node /BIN/TEST.JS` → ALL PASS

### 2.3 QEMU 实测 6/6 ALL PASS（8 张截图 r1~r8）

| # | 验收项 | 结果 |
|---|---|---|
| 1 | node 内嵌 demo（fib(10)=55、数组/对象/typeof） | ✅ `node: exit ok` |
| 2 | node /BIN/TEST.JS（函数/递归/while/数组/对象） | ✅ ALL PASS |
| 3 | test_ab v2.5 全功能回归（100KB 追加+偏移读+管道+spawn+delete） | ✅ ALL PASS |
| 4 | F1 回归：spawn_fail 40 次失败 spawn 后真实 spawn 成功 | ✅ SPAWNF ALL PASS |
| 5 | #48 回归：重启后 node 仍 ALL PASS、hello.ce 存活 | ✅ |
| 6 | 8 张截图齐全 | ✅ |

### 2.4 目标一：cc.ce 微型 C 编译器 —— QEMU 实机 4/4 PASS

- **产物**：`extapp/cc.c`（1,129 行 / 48,641B，C 子集编译器）+ `ce_stub.c`（__chkstk_ms 桩）+ `build_ce.sh` v2.5（-Iinclude/-fno-stack-check/-R .bss/--disable-reloc-section）→ `bin/cc.ce`（镜像 23,248B + bss 46,020B）
- **能力**：词法/递归下降解析/x86 栈式代码生成（EAX/ECX）/直接产出 CE01 镜像；支持算术、除模、for+break、while、递归（fib(10)=55）、全局 char* 字符串、比较、`api->` 内建调用
- **修复**（QEMU 首轮实测 `run /bin/OUT.CE` err -5 根因）：CE 加载器要求 entry==0 且 api_off+4<=image_size；cc 原生成 header entry=ce_main 偏移非 0 → 在 code 偏移 0 插 `E9 rel32` 跳板、header entry 恒 0，fixup_code 回填跳板 rel32
- **验收**：`run /bin/cc.ce /BIN/HELLO.C /BIN/OUT.CE` → OK；`run /BIN/OUT.CE` → `cc-compiled hello` + `fib(10)=55 sum(5)=15`；重启后 cc 存活、重编 OUT2.CE 运行 OK（#48 回归）

| # | 验收项 | 结果 |
|---|---|---|
| 1 | cc.ce 在 CatOS 内编译 HELLO.C（fib 递归 + while 求和） | ✅ `OK -> /BIN/OUT.CE` [exit(0)] |
| 2 | 编译产物 OUT.CE 运行 | ✅ `cc-compiled hello` / `fib(10)=55` / `sum(5)=15` |
| 3 | 重启后 cc.ce 存活（#48），重编 OUT2.CE | ✅ `OK -> /BIN/OUT2.CE` |
| 4 | OUT2.CE 重启后运行 | ✅ `cc-compiled hello` / `fib(10)=55` |

- 截图：`cc_shots/r1~r6`（r2 编译 / r3 产物运行 / r5 重启后重编 / r6 重启后运行）

### 2.5 目标一 P1 修复 + 回归（2026-10-30，双审闭环第二阶段）

- **触发**：功能/正确性双路只读审查（`cc-review.md`，判定 PASS / 0 P0 / 7 P1 / 8 P2），按用户指示直接修复 P1 清单。
- **cc.c 修复（6 处，kernel-v25 与 Catos_Better_better 双副本同步）**：
  1. FIND-04 for 的 continue 回填到 step 起点（无 step 回填到 top 条件）；同时修正无 step 的 for 循环体回跳目标（step_pos→top）
  2. FIND-05 `<<`/`>>` 插 `xchg eax,ecx`（87 C8）后再 shl/sar eax,cl，右操作数才进 cl
  3. FIND-01 parse 末尾 `g_entry_off==0 → err_set("no ce_main")`，非 0 退出不产出镜像
  4. FIND-02 新增共享 `str_decode()`：全局/局部字符串转义统一（\n \t \r \0 \\ \"）
  5. FIND-03 全局 `char arr[]="str"` 改为逐字节内联初始化（不再写指针偏移进数组槽）
  6. FIND-06 reloc/call/apiref 三表满容 `err_set`（不再静默丢弃）
  7. **DREL 数据区重定位**（回归新发现）：全局 `char *g_p="str"` 指针槽必须存**绝对地址**（parse 期 g_olen 未定 → 新增 g_drel_at/g_drel_off 表，fixup_code 按 `CE_LOAD_BASE+最终olen+off` 回填；宿主验证槽值 0x20002fd OK）
- **测试脚本**：`cc_qemu_test.py` check() OR→AND（全 needle 命中才 PASS）；新增 `EXTRA.C`（移位/for+continue/无 step for/全局串转义/数组串初始化）与 `NOCE.C`（无 ce_main 报错）用例。
- **内核修复（回归新发现）**：`task.c sys_alloc` 用户堆分配只映射首个 4K 页 → 多页分配第二页起 #PF（EXTRA.C 236 token 写第 171 个 token 即崩）；已修复为分配时映射 payload 全部页面，记入 `CatOS-内核问题清单.md` #47；`sh build.sh iso` 重建内核+ISO。
- **验收**：QEMU 全量回归 **8/8 PASS**——原 4/4（HELLO.C 编译/运行/重启存活/重编）+ EXTRA.C（`sh=12 t=8 c=3 z=12 msg=hi p1=A p2=B` 全对）+ NOCE.C（编译报错 `no ce_main`、不产出镜像）+ check() AND 语义；EXTRA.C step 用子集支持的 `i = i + 1`（子集不支持后缀 ++/--）；截图 `cc_shots/r1~r9`（9 张 png）。

---

## 三、Git 与 GitHub 状态

| 项 | 内容 |
|---|---|
| 本地 HEAD | `ba6a095` — go-review.md 终版（审查报告入 git） |
| GitHub ocean-2026/ocean-tools main | 最新 `4c322989`（清理 esc_test.txt 实验文件）——**源码推送闭环完成** |
| 工作树 | 近 clean（仅 cc-deliver/、cc.github.c、cc.min.c 三件备用/临时文件 untracked） |

**GitHub 源码推送闭环（2026-10-30，push_files 通道，全部经 curl raw.githubusercontent 逐字节核验）**：

| commit | 文件 | 核验 |
|---|---|---|
| `128e3ab` | kernel-v25/extapp/cc.c（51,502B） | ✅ IDENTICAL |
| `b66c8f70` | kernel-v25/src/task.c（61,406B） | ✅ IDENTICAL（前两版 027dd7c/9f34b559 有转写差异已覆盖） |
| `477554da` | kernel-v25/extapp/node.c（53,353B） | ✅ IDENTICAL |
| `96d21f79` | kernel-v25/extapp/go.c（43,498B） | ✅ IDENTICAL |
| `2b8dab8b` | TEST.GO/DEMO.GO/MINI_GO.GO/MINI_IN.GO/SYN.GO/go_qemu_test.py 六件 | ✅ 全部 IDENTICAL |
| `9e6be7f2` | GO-DESIGN.md | ✅ IDENTICAL |
| `37502e9c` | go-review.md（16533B） | ✅ IDENTICAL（两处表格转义 `\|`→中文"或"表述，消除通道反斜杠丢失） |
| 早前 | affbd11（cc_qemu_test.py+EXTRA.C+NOCE.C+cc-review.md）、b4f8889（PROGRESS/history/内核清单） | ✅ 已核验 |

经验：push_files 参数通道对**字面反斜杠**存在生成端丢失风险（大文本多次重推仍丢），长文本含转义符时改措辞规避最稳；通道本身经 esc_test.txt 实验确认可传 `\|`。

---

## 四、待办事项

### P0 —— 目标一：GNU 工具链 CatOS 自举（子集）【任务 #19】✅ 核心闭环完成

- [x] **微型 C 编译器（cc.ce）**：CE 用户态实现——词法/语法解析 → x86 栈式代码生成 → CE01 镜像
- [x] 编译 `HELLO.C`（递归 fib + while 求和）→ 生成可执行 OUT.CE，QEMU 实机运行 PASS
- [x] 重启存活：#48 回归（cc.ce 与编译产物均存活，重编重跑 OK）
- [ ] **自举环（分阶段扩展中，2026-10-31 阶段1-3 完成）**：cc.ce 编译 cc.c 自身。可行性统计——cc.c（1281 行）子集外语法使用量：`switch`×5 语句（43 个 case 标签）、`struct`×4（token_t/sym_t 等核心数据结构）、`typedef`×3、`enum`×3、`goto`×4、后缀 `++`×93/`--`×11、`#include`×1、`->`×131。**分阶段路线进度**：
  - [x] **阶段1 struct+typedef+enum**（2026-10-31）：st_find/st_member 结构体成员表、al_find/add_alias typedef 别名、c_find enum 常量、parse_struct_def/parse_typedef/parse_enum；gen_lvalue lvalue 地址链模型重构（`.`/`->`/`[idx]`/`&x`/sizeof）；STRUCT.C QEMU PASS（`x=7 y=10 gp=17 nm=hi A=0 B=1 C=10 D=11 sz=20`）
  - [x] **阶段2 switch/case**（2026-10-31）：比较链 codegen（case 常量表达式、default、多语句 case 体 jne 吃完整 case 体后再回填）；SWITCH.C QEMU PASS（`c1=100 c2=200 c5=500 c9=-1`）；妥协：无 break 时每 case 自动终止（不支持穿透，cc.c 各 case 均带 break 已覆盖）
  - [x] **阶段3 goto + 前后置 ++/--**（2026-10-31）：lbl_decl 标签表、goto 前后向跳转；do_incdec 前/后缀值语义；GOTO.C QEMU PASS（`s=10 i=5 post=3 i2=4 pre=5`）
  - [x] 阶段1-3 功能审查（cc-stage123-review.md：PASS/0 P0/1 P1/6 P2）；**FIND-S1-01**（struct char 数组成员 [idx] 赋值 store 尺寸错 4 字节 mov 覆盖相邻字段）已修复（gen_lvalue obj_sz 按元素宽重置），CHARBUG.C 回归 QEMU `x=42` PASS；修复轮 QEMU 12/12
  - [ ] 阶段4 剥离 #include/static const 依赖（cc.c 尚 1 处 #include + static 用法；全局数据区仍 8KB，自举需扩 g_data）
  - [ ] 阶段5 cc.ce 编译 cc.c 出 cc2.ce，cc2.ce 复编译自身字节级一致（自举不动点验证）
  - 每个阶段以回归用例+QEMU 实测收口。
- [x] 双审闭环（功能审查子代理，PASS/0 P0/7 P1/8 P2）+ P1 全部修复 + QEMU 8/8 回归
- [x] GitHub 推送（2026-10-30）：cc.c/task.c/node.c 源码经 push_files 通道分 3 个 commit 推送（见 Git 状态）

### P1 —— 收尾与打磨
- [x] 推送 `node.c` 到 GitHub（2026-10-30 push_files 通道，kernel-v25/extapp/node.c）
- [x] 版本号评估（2026-10-31）：本轮为纯用户态工具链变更（cc.ce 阶段1-3 + go.ce v1.2.1），内核 b31 无改动 → problem.md 保持 v2.5 系列不升 v2.6；工具链子版本在 PROGRESS 记录
- [x] CatOS-内核问题清单.md 第九节闭合表核对（2026-10-31）：#48（go 重启回归再验证）/F1（spawn_fail 实测）/A2（test_ab 全过）/ 已全部闭合；#3 随 #48 闭合

### P2 —— 后续扩展（可选）
- [ ] node 子集增强：字符串方法/更多内置对象/模块加载
- [ ] 语言环境目标二备选：python 子集（用户原选项，node 已先行）
- [ ] GNU 全量路线：as/ld 汇编链接器 → 后续 gcc 完整移植评估

---

## 五、需要用户介入的点

1. **目标一是否立即开工**：按"一次性完成"的要求，#19 是唯一缺口；建议下轮由父代理亲自实现（用户已授权"任务重你也可以自己上场"），或再委派新组织者。
2. **node.c 推送方式确认**：本地已有完整 commit，是否接受"仅文档上 GitHub、源码保留本地"的现状，或要求继续推源码。

---

*本文件随 0e99d76 之后的新 commit 一起维护；每次开发轮结束后更新本节状态。*

## Go 目标（go.c 用户态 Go 子集解释器）v1.0
- 产物：`extapp/go.c` → `extapp/bin/go.ce`（image 20348B, bss 152KB, entry=0, api_off=0x4f78 对齐）。
- 能力：词法/语法/求值分层；int/string/bool；var/:=赋值；算术/比较/逻辑；if-else；三段式/while 式 for；break/continue；函数+递归；数组下标读写；字符串拼接/sub/字节；print/len/readfile/argv/atoi/chr/string/append 内建。报错带行号。
- 自举：`MINI_GO.GO`（用 Go 子集写的迷你解释器）经 `readfile(argv(2))` 读入并解释 `MINI_IN.GO`（变量+算术+print），输出 `7`，与宿主预期一致。
- QEMU 验收（go_qemu_test.py，日志 /tmp/go_qemu_run1.log）：
  1. `run /bin/go.ce` → demo fib(10)=55 / demo done / exit ok —— PASS
  2. `run /bin/go.ce /BIN/TEST.GO` → 34 自检 + ALL PASS —— PASS
  3. 重启后 TEST.GO 仍 ALL PASS（#48 回归）—— PASS
  4. `run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO` → 输出 7 —— PASS
- 截图：`cc_shots/go_r1_shell..go_r6_selfhost`（.ppm+.png）。
- 详见 GO-DESIGN.md。

## go.c 双审 P1 修复（v1.1，QEMU go_fix 回归 5/5 PASS）
- FIND-G1 语法行号定位：Tok.line + perr 取当前 token 行；SYN.GO（第3行缺闭括号）实测报 `line 3`。
- FIND-G2 字符串拼接：去 tmp[200] 硬截断，arena 动态拼接，250 字符 len=250，"n="+42="n=42"。
- 新镜像头：CE01/entry=0/bss=0x292c0/api_off=0x5320/image=0x5324（api_off+4==image）。
- QEMU：demo fib=55 / TEST.ALLPASS / 重启 ALLPASS / 自举输出7 / SYN 行号=3，全 PASS；截图 cc_shots/go_fix_r1..r7。
- P2（G3~G10）8 项已列入 GO-DESIGN.md 迭代清单，本轮不修。

## cc.c 自举环阶段1-3（2026-10-31，QEMU 10/10 PASS）
- 阶段1 struct+typedef+enum：新增 stdef_t 结构体表(成员偏移/对齐)、alias_t 别名表、const_t 枚举常量表；表达式重构为 gen_lvalue 地址链模型(`[idx]`/`.`/`->`)；`&x` 取址修复；sizeof(类型/变量/解引用)。
- 阶段2 switch/case/default：比较链 codegen（mov eax,[esp]; cmp imm; jne 下一 case），break 复用 g_brk 作用域。修复 case 体多语句导致 jne 误回填到 break E9 的 bug。
- 阶段3 goto+前后置 ++/--：label: 与 goto（前向占位回填/后向直接 jmp_back）；do_incdec 统一前/后缀（后缀返回旧值）。
- cc.c：1280 → 1776 行（+549/-53）；cc.ce image=33780B bss=0x13aa4 api_off=0x83f0（CE01/entry=0 契约保持，未改任何头文件）。
- QEMU 实测（cc_stage_test.py，盘已 make_disk+format(y)+注入）：HELLO/EXTRA 回归 4/4 PASS；STRUCT `x=7 y=10 gp=17 nm=hi A=0 B=1 C=10 D=11 sz=20` PASS；SWITCH `c1=100 c2=200 c5=500 c9=-1` PASS；GOTO `s=10 i=5 post=3 i2=4 pre=5` PASS。
- 截图：cc_shots/stg_r1~r11（.ppm+.png，关键 r7_struct/r9_switch/r11_goto）。
- 遗留/妥协：① 不支持 case 穿透（无 break 时每 case 自动终止，符合 cc.c 各 case 均带 break 的用法）；② 成员不支持嵌套结构体成员再取成员（cc.c 成员仅 int/char/char[]/char*，已覆盖）；③ 全局结构体大数组仍受 g_data 8KB 限制（自举 cc.c 本体时需扩 g_data，留待阶段4-5）。
