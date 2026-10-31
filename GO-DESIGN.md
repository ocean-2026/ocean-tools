# GO-DESIGN.md — CatOS `go.ce` 用户态 Go 子集解释器

版本：v1.0（QEMU 实测 PASS）
参照：`extapp/node.c`（JS 子集解释器，CE 程序范本）
构建：`sh extapp/build_ce.sh go.c` → `extapp/bin/go.ce`

---

## 1. 目标
在 CatOS（GNU 工具链自举路径，目标一延续）提供一个**可自举的 Go 运行环境**：
一个跑在 CatOS 用户态的 CE 程序 `go.ce`，能用 Go 语法执行脚本，并能解释一个更小的 Go 子集程序（自举）。

## 2. CE 契约（v2.5，与 node.c/cc.c 完全一致）
- CE01 20B header：magic=`CE01`，`entry=0`（`ce_main` 在 `.text.ce_entry`，链接脚本置首）。
- api 槽在 image 末尾（`api_off+4 == image_size`），由 `build/mkce.py` 写入；48 槽与 `ce_abi.h`/`include/ce.h` 逐槽对齐，**未改任何头文件**。
- 构建产物头 v1.1（`xxd -l 20 bin/go.ce`）：
  ```
  4345 3031  magic="CE01"
  0000 0000  entry=0
  c092 0200  bss_size=0x000292c0 (168640)
  2053 0000  api_offset=0x00005320
  2453 0000  image_size=0x00005324 (21284)
  ```
  校验：`0x5320 + 4 = 0x5324 == image_size`。✓

## 3. 子集规格（v1.0）
分层：**词法(lexer) → 语法(parser, AST) → 求值(eval)**，与 node.c 同构。

| 类别 | 支持 |
|---|---|
| 注释 | `// 行` 与 `/* 块 */` |
| 字面量 | 十进制 int、字符串 `"a\nb"`（`\n \t \" \\ \0`）、`true/false`、字符 `'A'`（→ int，支持 `\n \t \r \0`） |
| 声明 | `var x = e`（可选类型名被跳过）；短变量 `x := e` |
| 赋值 | `x = e`、`a[i] = e` |
| 算术 | `+ - * / %`（32 位截断，`/` 向零取整）；字符串 `+` = 拼接 |
| 比较 | `== != < <= > >=`（字符串比内容） |
| 逻辑 | `&& || !` |
| 控制 | `if/else`、三段子句 `for i:=0;i<n;i=i+1{}`、while 式 `for cond{}`、`break/continue`、`return e` |
| 函数 | `func name(p1,p2){...}` 递归、闭包定义作用域 |
| 数组 | `[1,2,3]` 混合值数组；下标读写；`len(a)` |
| 报错 | 词法/语法错含**源行号**（`go: syntax error at line N`） |
| 容量 | 源缓冲 `g_filebuf[20480]`（≥8KB）；解释器 arena 经 `api->alloc` 从内核堆取 192KB（fallback 静态 64KB） |

内建函数（注册于全局作用域）：
`print(...)`（空格分隔+换行）、`len(s|a)`、`readfile(path)->string`、`argv(n)->string`、`atoi(s)`、`chr(n)->string`、`sub(s,start,n)`、`string(n)`、`append(arr,v)`。
字符串下标 `s[i]` → 字节 int。

脚本选择（首个命中）：`argv[1]` → `/BIN/DEMO.GO` → 内嵌 demo。

## 4. 自举定义与验收标准
**自举定义**：用 Go 子集本身写一个迷你解释器 `MINI_GO.GO`，它在 `go.ce` 下运行，并读取+解释另一个更小的 Go 子集程序 `MINI_IN.GO`（仅支持 `x = 数字`、`x = y op z`、`print x`，26 个字母寄存器）。

- `MINI_GO.GO` 用内建 `readfile(argv(2))` 读入目标源码，空白分词成 string 数组，逐 token 解释赋值/print。
- 验收：`run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO` 的输出**逐字节等于宿主预期**。
- `MINI_IN.GO`：
  ```
  a = 3
  b = 4
  c = a + b
  print c
  ```
  宿主预期输出：`7`。QEMU 实测屏显确认为 `7`（截图 `cc_shots/go_r6_selfhost.png`）。✓

## 5. QEMU 实测验收清单（全部 PASS，截图 cc_shots/go_rN.*）
| # | 命令 | 预期 | 结果 |
|---|---|---|---|
| 1 | `run /bin/go.ce`（无参） | demo：`fib(10)= 55`、`demo done`、`exit ok` | PASS（go_r2_demo） |
| 2 | `run /bin/go.ce /BIN/TEST.GO` | 34 项自检 + `ALL PASS` | PASS（go_r3_test） |
| 3 | 重启后重跑 TEST.GO | 仍 `ALL PASS`（#48 回归） | PASS（go_r5_test_after_reboot） |
| 4 | `run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO` | 输出 `7` + `exit ok` | PASS（go_r6_selfhost） |

TEST.GO 自检覆盖：算术/优先级、6 种比较、4 种逻辑、if/else、三段式 for、while 式 for、break/continue、fact/fib 递归、数组读写求和、字符串长度/sub/字节/atoi/chr/拼接。

## 6. 迭代清单（含双审 go-review.md FIND 修复状态）
### 已修复（v1.1，双审 P1 闭环，QEMU go_fix 回归 PASS）
- **FIND-G1（P1）语法错误行号不定位**：`Tok` 增加 `line` 字段，lex 时记录 token 起始行；`perr()` 取当前 token 行号（不再因 lex 一次扫完全文而冻结在文末）；未闭合 `(`/`(` call 缺 `)` 时改报开括号所在行。实测 SYN.GO（第 3 行缺 `)`）报 `line 3`。
- **FIND-G2（P1）字符串 `+` 拼接 199 字节硬截断**：删掉栈上 `tmp[200]`，改用 arena 按 `v_disp_len(l)+v_disp_len(r)` 动态分配拼接缓冲；数字/布尔操作数内联渲染。实测拼接 250 字符 `len=250`（原 199），`"n="+42 → "n=42"`。

### 已修复（v1.2，P2 ×8 全修，QEMU go_p2 7/7 + 复查 PASS）
- **FIND-G3** 递归栈深上限：`g_call_depth` 计数 >200 帧 `rerr("recursion too deep")`。
- **FIND-G4** 非法字符词法错：lexer default 分支 `perr`（不再静默吞成运行时 undefined variable）。
- **FIND-G5** readfile 16KiB 分块：`read_whole()` 按内核 file_read 单次上限 16384 循环合并；BIG.GO 18204B QEMU 实测读回完整。
- **FIND-G6** arena 子分配判空：scope_new/arr_new 的 a_alloc 判空。
- **FIND-G7** 字符串字面量扩容：lex_string 双 pass（先扫闭合 `"` 数解码长度 → arena n+1 分配 → decode），无 1023 截断；未闭合引号报错。
- **FIND-G8** 顶层 break/continue/return：g_loop_depth/g_func_depth==0 时 `perr("... outside ...")`。
- **FIND-G9** for init 循环作用域：S_FOR 执行时 scope_new 隔离，i 不泄漏外层。
- **FIND-G10** 数字溢出检测：uint32 扫描 >INT32_MAX 报错。

### 已修复（v1.2.1，复查 NEW-01 补修，QEMU go_p2b 9/9）
- **NEW-01（P2）n_new/a_str/a_strn OOM 安全路径**：G6 复查发现漏了 AST 主分配 `n_new`（650-700 条语句时 arena 耗尽解引用空指针段错误 rc=139）。v1.2.1：`a_str/a_strn` OOM 返 empty 哨兵、`n_new` 返 bad 哨兵并置 g_perr、ce_main 检 `g_oom` 打印 `go: out of memory (script too large for interpreter arena)` 返 1。LARGE.GO（49916B/1000 语句）QEMU 实测干净报错 rc=1，无段错误。

### 远期
- map/结构体、`range`、多行返回值、`fmt.Println` 包路径；真编译为 CE 产物；MINI_GO.GO 扩到完整表达式优先级与注释/字符串字面量；Go 脚本容量（g_filebuf 20KB）与 arena 192KB 扩容评估。
