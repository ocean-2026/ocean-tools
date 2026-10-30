# go-review.md — CatOS 目标一延伸（go.ce Go 子集解释器）功能/正确性全量审查

- 审查日期：2026-10-30
- 审查对象：`Catos_Better_better/Catos_Better_better/extapp/go.c`（1161 行，CE 用户态 Go 子集解释器）、`GO-DESIGN.md`、`TEST.GO`/`DEMO.GO`/`MINI_GO.GO`/`MINI_IN.GO`、`go_qemu_test.py`；契约侧 `extapp/ce_abi.h`、产物 `extapp/bin/go.ce`
- 审查方式：**只读静态逐行审查 + 宿主侧独立复验**。宿主侧用遗留 `host_stub_go.c`（stdio/文件系统桩）以当前 `go.c` 原生 64 位 gcc 重编译为 `go_host_fresh`，跑 DEMO / TEST.GO / 自举链。**未修改 go.c 或任何被测源、未运行 QEMU、未重建 go.ce 镜像**；QEMU 4/4 PASS 与截图证据作为外部事实采信。
- 判定尺度：P0=验收链失败/崩溃；P1=受支持子集产出错误或危险静默失败（不在验收路径上）；P2=未宣传构造/边缘限制/健壮性瑕疵。

---

## 总体判定：**PASS（功能/正确性验收链）**

- **无 P0**。词法/语法/求值三层、作用域与递归、内建与 48 槽 API 对齐、arena 内存安全、自举诚实性、CE 镜像契约全部成立，与 QEMU 实测 4/4 PASS 互证。
- **2 个 P1**（均不在验收路径上：语法错误行号不定位、字符串拼接 199 字节静默截断）；**8 个 P2**。
- 宿主独立复验三项输出与开发子代理宣称**逐行一致**（见末节）。

---

## 宿主侧独立复验结果（go_host_fresh，原生 gcc 编译当前 go.c）

| 链 | 命令 | 宿主实测输出 | 判定 |
|---|---|---|---|
| DEMO | `go_host_fresh`（无参→/BIN/DEMO.GO） | `x= 5  y= 11  s= hello go` / `fib(10)= 55` / `add(3,4)= 7` / `total= 10  evens= 6  a[2]= 3` / `demo done` / `go: exit ok` | ✅ 与宣称一致 |
| TEST.GO | `go_host_fresh /BIN/TEST.GO` | 34 行 `PASS ...`（arith×6 / cmp×6 / logic×4 / if-else×2 / for-sum / while-sum / break-continue / fact-5 / fib-10 / fact-1 / arr×4 / str×6）+ `ALL PASS` + `go: exit ok` | ✅ 34/34 ALL PASS |
| 自举 | `go_host_fresh /BIN/MINI_GO.GO /BIN/MINI_IN.GO` | 中间输出仅一行 **`7`** + `go: exit ok` | ✅ 自举输出=7 |

hostroot 内四个 `.GO` 与 `catos-qemu/` 权威源 `diff` 全部 IDENTICAL，复验未使用过期脚本。

---

## A. 词法 / 语法层

| 检查点 | 结论 | 证据 |
|---|---|---|
| 标识符 intern（NUL 结尾副本） | ✅ | lex 期 T_ID.sp 指向源缓冲（非 NUL），但 `p_primary` go.c:443 与 var/func/形参处（:504/:520/:526/:533）均经 `a_strn(sp,len)` 拷入 arena 且 `sncpy` 后补 NUL（:50-53）；`scope_find` 用 `scmp`（strcmp）依赖的正是这些 NUL 副本，自洽 |
| 字符串转义 `\n \t \" \\ \0` | ✅ | `lex_string` go.c:218-225 switch 命中 n/t/"/\\/0，共 5 种，与 GO-DESIGN §3 表所列 5 种一致（未宣称 `\r` 入串） |
| 字符字面量 `'A'`/`\n \t \r \0` | ✅ | `lex_char` go.c:240-241 支持 n/t/r/0；MINI_GO.GO:23 用 `c == '\r'` 依赖此点，宿主跑通 |
| `!` 一元 vs `!=` 二元 | ✅ | lex go.c:293-296：下一字符为 `=`→OP_NE，否则→`!`；`p_unary` go.c:409 仅认 `!`，无歧义 |
| `:=` 短变量 | ✅ | lex go.c:310 把 `:=`/`:` 都映射为 `:` op；`p_stmt` go.c:615-620 用 `tok_is_op(':')` 判 shortv，`=` 走普通赋值 |
| 比较链 `a < b < c` | ✅ 可接受 | `p_cmp` go.c:377-389 左结合循环解析，宽容但不产出错误 |
| 优先级（precedence climbing） | ✅ | or→and→cmp→add→mul→unary→postfix→primary（go.c:359-467），TEST.GO `2+3*4=14`、`(2+3)*4=20` 实测正确 |
| **词法/语法错带行号** | ⚠️ **不定位（P1，见 FIND-G1）** | 见下 |

**FIND-G1（P1）——语法错误行号恒为"文件总行数"，不定位错误行。**
- 根因：`lex_run`（go.c:261-316）在解析前**一次性扫完全文**并推进 `g_line`；解析阶段 `perr()`（go.c:344-347）捕获的 `g_line` 已冻结在文末行号。`Tok` 结构（go.c:166-171）**没有 per-token line 字段**。报错处 go.c:1135-1136 输出 `g_errline`（= 文末）或 `g_line`（= 文末）。
- 实测（host）：6 行脚本中第 5 行 `var e = (1 + 2`（缺右括号），报 `syntax error at line 7`；3 行脚本缺 `}`，报 `line 4`。两者都是"行数+1"，与错误实际位置（5、3）无关。
- 影响：GO-DESIGN §3 明示"词法/语法错含源行号（go: syntax error at line N）"——该功能**名不副实**，N 对定位无帮助。不影响验收链（验收脚本无语法错）。
- 一行级修复（交开发子代理）：给 `Tok` 加 `int line` 字段，`lex_add` 时盖当前 `g_line`；`perr()` 改为 `g_errline = cur()->line`（错误发生时当前 token 的行）。

**P2 附记**：非法字符（如 `@`）落入 lex 默认分支 go.c:313 `g_pos++; break;` 被**静默丢弃**，把一个词法错延迟成运行期 `undefined variable`（实测 `var e = @bogus` → "runtime error: undefined variable" 而非 syntax error）。建议 default 分支 `perr`。

---

## B. 值模型

| 检查点 | 结论 | 证据 |
|---|---|---|
| int 算术 + 除/模零保护 | ✅ | go.c:886-892；`r.num==0` 时 rerr 并返 0（:889/:891），TEST `20/6=3`、`20%6=2` 通过 |
| 比较（int / 字符串内容） | ✅ | go.c:893-912；字符串 ==/!= 逐字节比（:894-908），TEST 6 种比较全 PASS |
| 逻辑 `&&`/`\|\|` 短路 | ✅ | go.c:872-873：左值定真/假即返回，不求值右值；`1&&0=0`、`0\|\|1=1` 通过 |
| `!` 取反 truthy | ✅ | go.c:866 `!truthy(x)`；`!0=1`、`!1=0` 通过 |
| 字符串拼接与长度 | ⚠️ **199 字节硬截断（P1，FIND-G2）** | 见下 |
| 数组下标读（越界报错） | ✅ | go.c:849-856：V_ARR 越界 `rerr("array index out of range")`；写路径 go.c:766 同样检查 |
| 字符串下标 `s[i]`→字节 | ✅ | go.c:857-859 返回 `unsigned char`；越界静默返 0（:858，与数组报错策略不一致，但 TEST/自举均在界内） |
| 值拷贝 vs 引用 | ✅ by design | GVal.op 为数组/函数指针，数组赋值/传参为**浅引用共享**（append 原地长大），MINI_GO 的 `reg[slot]=...` 依赖此语义，实测正确 |

**FIND-G2（P1）——字符串 `+` 拼接在栈上 `tmp[200]` 内硬截断到 199 字节，静默丢数据。**
- 根因：go.c:878-884 字符串拼接分支用 `char tmp[200]`、`k<199` 限制，超出部分直接丢弃，再 `a_str(tmp)`。
- 实测（host）：`for i<250 { s = s + "x" }` 后 `len(s)` = **199**，不是 250。
- 影响：GO-DESIGN §3 把字符串 `+` 宣传为拼接、未声明长度上限。自举 MINI_GO 的分词器 `cur = cur + chr(c)`（MINI_GO.GO:29）逐字符累积——若目标源码出现 >199 字符的无空白 token，`cur` 会被静默截断导致解释结果错。验收脚本 token 均 ≤1 字符，不受影响；但这是**受支持操作的静默错编**类缺陷（与 cc-review 的 P1 同类）。
- 一行级修复（交开发子代理）：删掉 `tmp[200]`，改为 `int k = l.slen + r.slen; char *p = a_alloc(k+1); memcpy 两段; return v_str(p,k);`（arena 已具备动态分配能力）。

---

## C. 求值器

| 检查点 | 结论 | 证据 |
|---|---|---|
| 语句/表达式分层 | ✅ | S_* 语句（go.c:130-133）与 E_* 表达式（:134-137）分 AST；`exec_stmt` 与 `eval_expr` 分离 |
| 作用域（全局/函数局部/遮蔽） | ✅ | `g_globals` + 每函数 `scope_new(f->scope)`（go.c:943），`scope_find` 沿 parent 链（:641-646），闭包记录定义时 scope（:804） |
| 函数定义+调用+递归 | ✅ | `call_user` go.c:941-959 建局部 scope、置参、exec 函数体、读 `g_ret`；TEST fact(5)=120、fib(10)=55、fact(1)=1 通过 |
| 三段式 for | ✅ | go.c:815-826：init(:816)→cond 判(:819)→body(:820)→flow→post(:824)；TEST `for i:=1;i<=5;i=i+1` sum=15 通过 |
| while 式 for / 裸 for | ✅ | 解析 go.c:591-597（无 `;` 则为 cond 式）；TEST `for j<5` twos=10 通过 |
| break/continue 目标定位 | ✅ | 单一 `g_flow`（1=break/2=continue/3=return）被**最近一层 for** 消费并清零（go.c:821-823）；TEST break@5 + continue@2 → bsum=0+1+3+4=8 通过 |
| return 路径 | ✅ | go.c:830 置 `g_ret`+`g_flow=3`；for 循环 :823 与 `exec_list` :774 均据此停跑，`call_user` :954 取回 |

**P2 附记**：
- **FIND-G3 无递归深度上限**：Go 递归每级都嵌套一层 C `call_user`（go.c:953→:941），无限递归（`func f(){f()}`）会撑爆 C 调用栈——CE 仅 64KiB 用户栈，会缺页杀任务；arena OOM 能挡住内存但挡不住栈深。建议加一个 `g_call_depth` 计数（>200 帧 `rerr`）。验收递归深度 ≤10，安全。
- **FIND-G8 顶层 break/continue/return 静默吞掉**：顶层 `exec_list`（go.c:774）见 `g_flow!=0` 即停跑后续语句，但不报错，也不复位。属脚本编写错误被静默忽略。
- **FIND-G9 for init 的 `:=` 变量泄漏外层作用域**：go.c:816 直接在当前 scope declare（如 `i`），非 Go 的循环作用域；无害但语义略宽。

---

## D. 内建与 API（ce.h 48 槽逐一对齐）

| 内建 | bid | case | 结论 |
|---|---|---|---|
| `print(...)` | 0 | go.c:969 | ✅ 空格分隔+换行 flush（:710） |
| `len(s\|a)` | 1 | :975 | ✅ 字符串/数组两分支 |
| `readfile(path)` | 2 | :981 | ✅ 失败返 `""`（:985） |
| `argv(n)` | 3 | :989 | ✅ 边界检查 n<0||n>=argc 返 `""`（:993） |
| `atoi(s)` | 4 | :997 | ✅ 转发 `api->atoi` |
| `chr(n)` | 5 | :1001 | ✅ 取低字节成 1 字节串 |
| `sub(s,st,n)` | 6 | :1005 | ✅ 越界钳制（:1008-1010） |
| `string(n)` | 7 | :1013 | ✅ 十进制转串 |
| `append(arr,v)` | 8 | :1018 | ✅ 原地长大返原数组 |

注册名表 go.c:1103-1104 与 case 0..8 **顺序、数量完全对齐**。

**api-> 槽位使用核对（对照 ce_abi.h 48 成员）**：go.c 实际调用 `api->write`(槽0)、`putc`(槽1)、`print_int`(槽14)、`alloc`(槽9)、`atoi`(槽13)、`file_exists`(槽24)、`file_read`(槽39)——全部在 48 槽内且成员名/类型一致（48 成员清单已逐一数出：write…wait，含 v2.5 的 file_read/write/pipe_*/spawn/wait）。go.c 未用 `free`（arena 一次性 bump，符合设计）。

**FIND-G5（P2）——readfile 源缓冲 20KB 与内核 file_read 单次 16KB 上限的名义落差。**
- go.c:984/:1081 请求 `sizeof(g_filebuf)-1 = 20479` 字节；但 ce_abi.h:98 注释 `file_read ... max <= 16384 per call`。内核侧若按 16384 钳制/返回 -3，则 **>16KB 的源脚本或 readfile 目标会被拒/截断**（got<0→`""` 或 "cannot read script"）。
- 验收脚本：DEMO 592B、TEST 2345B、MINI_GO 2088B、MINI_IN 30B，均远 <16KB，不受影响。GO-DESIGN §3 写"源缓冲 20480（≥8KB）"——缓冲属实，但单文件实际可加载上限约 16KB。建议文档注明或改用分段 file_read。

---

## E. 内存安全

| 检查点 | 结论 | 证据 |
|---|---|---|
| arena 边界（api->alloc 192KB） | ✅ | `a_alloc` go.c:81-87 8 字节对齐+越界置 `g_oom` 返 0；主路径 `n_new`/`a_str`/`scope_new` 均判空 |
| 字符串 NUL 终止 | ✅ | lex_string :230、a_strn :96、readfile :986 均补 0；GVal 输出用 `slen` 不依赖 strlen（:742） |
| 输出缓冲 `g_out[256]` | ✅ | `out_pushc` go.c:700 以 `sizeof-1` 为界，越界不写 |
| 路径缓冲 `g_pathbuf[256]` | ✅ | go.c:983 `sncpy(...,sizeof-1)` |
| 源缓冲 >20KB 行为 | ✅ 不越界 | loader 以 max=20479 限读，`g_filebuf[got]=0`（:1086）；不会写穿 |
| g_filebuf 复用安全 | ✅ | 解析期 T_STR（:231）与 T_ID（:443）都已拷入 arena；执行期 readfile 覆写 g_filebuf 不影响 AST 引用 |

**FIND-G6（P2）——arena 扩容器分配未判空**：`scope_new` go.c:638 `s->v = a_alloc(...)`、`arr_new` :668 `a->v = a_alloc(...)` 均未判 NULL；若 arena 耗尽，`s->v/a->v=0`，后续 `scope_declare`/`arr_push` 解引用空指针。192KB arena 在验收量级内不会触发，属健壮性瑕疵。

**FIND-G7（P2）**：字符串字面量 >1023 字符被 `lex_string` 的 `buf[1024]` 静默截断（go.c:213-214）；未闭合引号也不报错（:229 仅跳过）。

---

## F. 自举样例诚实性核验（对照 GO-DESIGN §4）

逐条核对设计文档的自举定义与验收标准：

| 设计文档声明 | 核验 |
|---|---|
| 用 Go 子集写 MINI_GO.GO，在 go.ce 下运行 | ✅ MINI_GO.GO 全文 2088B，确为 Go 子集语法（func/for/if/数组/append/readfile/argv），非 C/非桩 |
| readfile(argv(2)) 读目标源码、空白分词、逐 token 解释 | ✅ MINI_GO.GO:14 `readfile(argv(2))`；:21-35 空白分词成 `toks`；:48-79 逐 token 解释 |
| 仅支持 `x=数字`/`x=y op z`/`print x`、26 字母寄存器 | ✅ :15 建 26 元 `reg` 数组；:56 `slot=w[0]-'a'`；:61 仅认 `+ - *` |
| MINI_IN.GO 内容 `a=3 b=4 c=a+b print c` | ✅ 与 catos-qemu/MINI_IN.GO 逐字节一致 |
| 验收输出=7 | ✅ **宿主独立复验输出仅一行 `7`**（非硬编码、非 echo）；手推 token 流：a=3→b=4→c=a+b=7→print c=7，闭合 |
| 不夸大 | ✅ §6 已如实列出"MINI_GO 不识别注释/字符串字面量、仅极简目标语言"——自举是"Go 子集程序解释一个更小语言"，**非 go.c 自我解释**，定义不含糊、未拔高 |

**结论：自举为真，未发现硬编码输出或夸大表述。**

---

## G. CE 镜像契约（xxd -l 32 实测 bin/go.ce）

```
00000000: 4345 3031 0000 0000 c052 0200 784f 0000  CE01.....R..xO..
00000010: 7c4f 0000 5589 e5...                         |O.....
```

| 字段 | 实测值 | 判定 |
|---|---|---|
| magic | `43 45 30 31` = "CE01" | ✅ |
| entry | `0000 0000` = 0 | ✅（`ce_main` 在 `.text.ce_entry`，go.c:1062） |
| bss_size | `0x000252c0` = 152256 | ✅ 与 GO-DESIGN §2 一致 |
| api_offset | `0x00004f78` = 20344 | ✅ |
| image_size | `0x00004f7c` = 20348 | ✅ |
| `api_off+4 == image_size` | 0x4f78+4 = 0x4f7c | ✅ 等号成立 |
| 文件尺寸 | 20368 = image_size + 20(头) | ✅ |
| 48 槽对齐 | ce_abi.h 逐一数出 48 成员 | ✅ |
| CE_MAX_IMAGE 上限 | 20348 ≪ 256KiB | ✅ |

镜像契约全部通过，与 GO-DESIGN §2 所列字节完全吻合。

---

## FIND 汇总

| 级别 | 编号 | 内容 | 位置 | 验收路径影响 |
|---|---|---|---|---|
| **P0** | — | 无 | — | — |
| **P1** | FIND-G1 | 语法错误行号恒为文件总行数，不定位错误 | go.c:345,1136；Tok 无 line 字段 | 否（验收脚本无语法错） |
| **P1** | FIND-G2 | 字符串 `+` 拼接栈上 tmp[200] 硬截断 199 字节、静默丢数据 | go.c:879-883（实测 250→199） | 否（验收 token 均短） |
| **P2** | FIND-G3 | 无 Go 递归深度上限，无限递归撑 C 栈 | go.c:941-953 | 否（递归深 ≤10） |
| **P2** | FIND-G4 | 非法字符被词法 default 静默吞成运行时错 | go.c:313 | 否 |
| **P2** | FIND-G5 | readfile 请求 20479 但内核单次上限 16384，>16KB 脚本被拒 | go.c:984,1081 vs ce_abi.h:98 | 否（脚本 <3KB） |
| **P2** | FIND-G6 | arena 扩容 `s->v`/`a->v` 未判空 | go.c:638,668 | 否（192KB 够用） |
| **P2** | FIND-G7 | 字符串字面量 >1023 静默截断、未闭合引号不报错 | go.c:213-229 | 否 |
| **P2** | FIND-G8 | 顶层 break/continue/return 静默停跑不报错 | go.c:774,828-830 | 否 |
| **P2** | FIND-G9 | for init `:=` 变量泄漏外层作用域 | go.c:816 | 否（无害） |
| **P2** | FIND-G10 | 数字字面量溢出无检测（int32 UB） | go.c:268 | 否 |

**结论**：
1. **go.ce 功能/正确性验收链 PASS，无 P0**——词法/语法/求值、作用域与递归、9 个内建与 48 槽 API、arena 安全、自举诚实性、CE 镜像契约全部成立，且宿主独立复验与 QEMU 实测宣称一致。
2. 2 个 P1（行号不定位、拼接 199 截断）均**不在验收路径上**，但属"受支持功能名不副实/静默错编"类，建议进目标二前修复（一行级方案已给出，交开发子代理，本审查未改任何代码）。
3. 8 个 P2 为健壮性/容量边缘项，可择期处理。

## 方法声明

本审查为**只读静态逐行审查 + 宿主侧独立复验**：以当前 `go.c` 原生 gcc（64 位）配合 stdio/文件系统桩重编译为独立 host 可执行，实测 DEMO、TEST.GO（34/34 ALL PASS）、自举链（输出 7）；以 xxd 实测 go.ce 头 20 字节；以对照实验定位行号与拼接截断两个缺陷。**未修改 go.c 或任何被测文件、未运行 QEMU、未重建 go.ce 镜像**；QEMU 4/4 PASS（含重启存活、截图证据）作为外部事实采信。
