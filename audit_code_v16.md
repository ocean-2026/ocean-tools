# ocntools.ce v1.6 代码审查报告（audit_code_v16）

- **审查对象**：`ocean_tools.c`（v1.6 主源，3721 行）、`hexedit_port.c`（745 行）、`fmgr_port.c`（747 行）、`build.sh` / `ce.ld` / `mkce.py`、外置脚本 `/BIN/DIFF.CSC`、`/BIN/VK.CSC`（disk.img 权威版，与 `/tmp` 版本逐字节一致，注入源目录仅 DEMO.CSC）
- **审查方式**：全程只读（未修改任何文件、未运行 QEMU）；主源通读 + grep 交叉验证 + 实际运行 `bash build.sh`；disk.img 仅经 fatfs.py 只读路径打开
- **审查时间**：2026-10-22
- **审查分工**：Part 1 主源文件 + 构建验证；Part 2 外置脚本与解释器语义核对

---

## 总体结论：**不通过**（存在 2 项高危阻塞项，修复并回归后方可交付）

构建层面通过（ocntools.ce = 59896B ≤ 64KiB，编译 0 error），CSC 2.0 解释器主体、已知三处修复点、外置命令机制、diff/plot 移除完整性全部验证通过；但存在 **1 项栈溢出风险**（执行任意 .csc 即触发）与 **1 项 rows 解析缺陷**（导致 DIFF 输出静默漏行），均需修复。

### 阻塞项清单（高危）

| # | 位置 | 问题 | 修复建议 |
|---|---|---|---|
| B1 | ocean_tools.c:587–592（run_script_args） | 栈帧约 21.4KB：`buf[8192]` + `lines[CSC_MAXLINES][CSC_LINEMAX+1]`（128×97） + `lbl_name[128][16]` 全部在栈上；而 3066 行自述内核栈仅 **8 KiB**。执行任何 .csc 文件即存在踩栈风险 | 改为文件级 `static` 缓冲（run_script 串行、csc_clear 保证不重入，安全） |
| B2 | ocean_tools.c:1011（rows/row 解析） | `u32 ci = (t[1]=='o' && t[2]=='w') ? 3 : 4;` —— 对 `rows ` 与 `row ` 恒为真，ci 恒=3；`rows ` 的 t[3] 是残留字母 's'，首 token 变成 "s"，缓冲字母被丢弃 → `rows B nb` 实际取到 fa 行数；DIFF 在 B 比 A 长时静默漏打尾部差异行 | 改为 `(t[3]=='s') ? 4 : 3;`；脚本写法无需改动 |

### 其余 FAIL 汇总（中/低）

| # | 级别 | 位置 | 问题 |
|---|---|---|---|
| M1 | 中 | ocean_tools.c:470–471 csc_save_shards `path[64]` | 长路径（write 源 `rp[128]`）+ 内容 >512B 时 my_strcpy 越界；建议 path 加宽或加长度门槛 |
| M2 | 中 | ocean_tools.c:2167/2332/2429/2466/2562 `vcs base[96]` | resolve_path 产物最长约 127 字符拷入 96 字节再拼 "/Fxx"；建议加宽至 160 |
| L1 | 低 | ocean_tools.c:3599–3605 alias_expand `newline[120]` | 别名体 95B + 参数 78B 可越界 |
| L2 | 低 | ocean_tools.c:809–826 | next 不校验循环变量名（功能正确，健壮性欠缺） |
| L3 | 低 | fmgr_port.c:84 `p[-1]` | 有 `p>tf_path` 守卫，运行时安全；gcc 保守告警 |
| L4 | 低 | ocean_tools.c:1002 | load/loadb 文件 >8K 静默截断，建议加提示 |
| L5 | 低 | ocean_tools.c:1973/1985 | 过时注释仍提 diff/vk/plot |
| L6 | 低 | DIFF.CSC / VK.CSC 注释 | 未声明 128 行硬上限、数组 64 项上限（行为安全，文档欠缺） |

### 整合说明

Part 1 总览中 rows/row 判 PASS 针对"变量名风格参数 + row 越界返回空串"；rows 关键字跳读长度缺陷由 Part 2 以 ocean_tools.c:1011 精确证据检出（B2），两者覆盖不同方面，不冲突。B2 已由上级复核源码确认。

---

## Part 1 主源文件审查与构建验证

审查范围：ocean_tools.c（3721 行，本次改动核心）、hexedit_port.c（745 行，#include 进主源）、fmgr_port.c（747 行）、build.sh / ce.ld / mkce.py。审查方式：只读通读 + grep 交叉验证 + 实际运行 `bash build.sh`（未运行 QEMU）。未修改任何源文件。

### ① 总览（逐项 PASS/FAIL）

**A. CSC 2.0 解释器**

| 检查项 | 判定 | 依据行号 |
|---|---|---|
| set：普通变量赋值 | PASS | csc_set 348–401；调用点 684 |
| set：数组元素 `set a[i]=V` | PASS | 名字解析 654–660 在 `[` 处截断；数组分支 663–680；下标展开 669–673；csc_arr_set 上下界保护 339–341 |
| set：算术 + - * / % | PASS | 361–389；**除零保护 369** `if (vb==0 && (op=='/'||op=='%')){ outln("csc: division by zero"); return; }` |
| for/next：4 层循环栈 | PASS | CSC_FORMAX=4（269）；栈数组 282–283 |
| for/next：栈越界保护 | PASS | 794 `>=CSC_FORMAX` 报错 break；810–823 空栈 "next without for" |
| for/next：next 与 for 匹配 | 部分PASS | 809–826 栈顶匹配/回跳正确；**未校验 next 变量名**（见 F-L-2） |
| len/upper/lower/substr/find/tok/same 变量名引用风格 | PASS | S/VAR 直接 csc_get 不展开（839/855/887/913/941/976）；数值参数展开（882/912/939） |
| substr/find 边界语义 | PASS | substr pos<0→0（886）、`pos+i3<L`（888）；find 未命中 "-1"（915） |
| load/loadb：8K 双缓冲 fa/fb | PASS | 278；isb 选择 997 |
| load/loadb：行偏移索引 | PASS | 1004 调 csc_index_lines（486–495） |
| load/loadb：缺失/超 8K | PASS（附注） | 缺失报错 1002–1003；>8K 静默截断（F-L-4） |
| rows/row：变量名风格/row 越界 | PASS | T=A/B 选缓冲（1024）；越界返回空 502（注：rows 关键字跳读缺陷见 Part 2 B2） |
| write：≤512B 分片 .A00 | 部分PASS | 459–483；**≤512B 直接写原 fname（463–465），>512B 才写 .A00/.A01**（467–481），语义自洽 |
| hex 转换语义 | PASS | 1070–1085；csc_hex 大写十六进制 513–531 |
| 位置参数 $0–$9 | PASS | csc_get 302–306：未传返回 ""；装载 602–609 |
| goto/label 匹配/流控制/定义 | PASS | 预扫 622–637；运行时跳过 `:label` 647；goto 707/if-goto 754/same 979 查表回跳 |

**B. 已知修复点**：① set 名字在 `[` 截断 — **PASS**（656）；② ext_scan 注册名转小写 — **PASS**（3391–3395）；③ comp_ctx names 48 — **PASS**（3361、3365）。

**C. 外置命令机制**：ext_scan 扫 /bin/*.csc 去扩展名注册 PASS（3371–3401，EXT_MAX=16 保护 3382）；分派顺序 内置TABLE(3624)→外置表(3639)→cmd_unknown(3664) PASS；run_script_args 参数传递 PASS（3649–3653/1941–1947/1961–1965）；Tab 补全覆盖内置+外置 PASS（3423–3443）；help 底部列外置命令 PASS（1390–1397）。

**D. 移除内置 diff/plot**：grep `cmd_diff\|cmd_plot\|td_\|tp_` 三文件 0 命中（仅 http_get 误匹配）；TABLE 3257–3303 无 diff/vk/plot 项；hexedit_port.c/fmgr_port.c 仅用自洽 thex_/tf_/tfm_ 符号，无悬空引用 — **PASS**。残留：1973/1985 注释仍提 "diff vk"/"plot"（过时注释，F-L-5）。

**E. 硬性环境限制**：镜像 ≤64KiB **PASS**（实测 59896B）；无浮点 PASS（仅 hexedit_port.c:255 关键字表字符串）；不依赖 api->exit PASS（全工程从未调用，exit 家族设 g_quit，ce_main return 0 @3720，3717 注释记录 kernel bug）；链接基址 0x02000000 无重定位 PASS（ce.ld:5、objdump 实测 ce_main@0x02000000）；CE01 头 PASS（magic/entry=0/bss/api_offset 全字段实测正确）。

**F. 通用代码质量**：malloc 失败均判空 PASS；off-by-one/未初始化 PASS；8K 截断受控；**缓冲区边界 FAIL 3 处、栈帧 FAIL 1 处（见下）**。

### ② FAIL 明细

- **【高】F-H-1 run_script_args 栈帧 ≈21.4KB vs 自述 8KiB 内核栈**（587–592：buf[8192]+lines[128][97]+lbl 表 ≈21368B 全在栈上；而 3066 写明 "kernel stack: 8 KiB"）。执行任何 .csc 即踩栈风险。建议改 static 文件级缓冲（run_script 串行、csc_clear 保证不重入）。
- **【中】F-M-1 csc_save_shards path[64]**（470–471）：长路径（write 源 rp[128]）+ >512B 内容时 my_strcpy 越界。建议 path 加宽 160 或加长度门槛。
- **【中】F-M-2 vcs base[96]**（2167/2332/2429/2466/2562）：resolve_path 产物最长 ~127 字符拷入 96 字节再拼 "/Fxx"。建议加宽 160。
- **【低】F-L-1 alias_expand newline[120]**（3599–3605）：别名体 95B+参数 78B 可越界。
- **【低】F-L-2 next 不校验循环变量名**（809–826）。
- **【低】F-L-3 fmgr_port.c:84 `p[-1]`**：有 `p>tf_path` 守卫，运行时安全，gcc array-bounds 保守告警。
- **【低】F-L-4 load/loadb >8K 静默截断**（1002），建议加提示。
- **【低】F-L-5 过时注释**（1973/1985）。
- 编译告警 7 条（不阻塞）：454/3370/3677 Wcomment、913 const 丢弃、hexedit_port.c:179 未用函数、137 my_tolower 未用、fmgr_port.c:84（=F-L-3）。

### ③ 构建验证结果

- **ocntools.ce = 59896 B（raw 59876 + 16B 头）≤ 65536 → PASS**
- **编译 error = 0**（build.sh exit=0）；**warning = 7**（记录不阻塞）
- 链接基址：ce_main @ 0x02000000（`02000000 <ce_main>: cmpl $0x0,0x200e9e0`），平坦 ld + objcopy -O binary 无重定位
- CE01 头实测（xxd）：`43 45 30 31`="CE01"，entry=`00000000`（偏移 0），bss_size=0x0000f9e4(63972)，api_offset=0x0000e9e0，image_size=0x0000e9e4(59876)
- bss 63972B 不占镜像限额。

---

## Part 2 外置脚本语义核对（DIFF.CSC / VK.CSC × 解释器）

### 0. 三处脚本来源内容差异说明

| 来源 | DIFF.CSC | VK.CSC |
|---|---|---|
| ① disk.img `/BIN/DIFF.CSC`（cl365, 507B） | 权威版 | — |
| ① disk.img `/BIN/VK.CSC`（cl366, 675B） | — | 权威版 |
| ② `/tmp/diff.csc`（507B） | 与①逐字节一致 | — |
| ② `/tmp/vk.csc`（675B） | — | 与①逐字节一致 |
| ③ 注入源 `ocean_tools/` | 仅 `DEMO.CSC`，**无 diff.csc** | **无 vk.csc** |

三处无差异（①==②）；注入源目录两脚本均不存在。另记录：`/BIN/` 内有两个 OCNTOOLS.CE（cl707=59896B v1.6 新镜像，cl593=58040B v15 旧镜像）及 OCNTV15.CE，与本核对无关。

### 1. 解释器语义基线实测（ocean_tools.c）

脚本行上限128(261,613-621)、行长96(262,616)、变量32(263)、标签32预扫描(264,622-637)、步数200000(265,643)、数组4个×64项**0基**(266-267,317-346)、`$1..$9`(268,297-308)、for栈4层(269,769-826)；文件双缓冲 fa/fb[8192]，cat只读8191B，行偏移索引上限128(278-280,486-495,989-1008)；行越界返回空串不报错(497-510)；set 整型 `+ - * / %`、两操作数皆数字才运算、除零打印并放弃赋值、无浮点(348-401，除零保护369)；if 双数字走数值否则字符串比较(554-578)；外部命令 ext_scan 扫 /bin/*.csc 小写注册，**内置 TABLE 已无 diff/vk**(3255-3300,3371-3401,3639-3663)。

### 2. DIFF.CSC 核对表（行号=disk.img 权威版）

| 行 | 构造 | 实现(源码行) | 结论 |
|---|---|---|---|
| 1-2 | `#`注释 | 647 | PASS |
| 3 | `load $1`→fa | 989-1008,486-495 | PASS |
| 4 | `loadb $2`→fb | 997-999 | PASS |
| 5 | `rows A na` | 1025-1028 | **FAIL(B2)**：ci错位，A=fa 恰好正确 |
| 6 | `rows B nb` | 1025-1028 | **FAIL(B2)**：nb 被赋成 fa_n 而非 fb_n |
| 7 | `set i=0` | 348-401 | PASS |
| 8 | `:cmp` | 622-637 | PASS |
| 9 | `if $i>=$na goto tailB` | 712-761,554-578 | PASS |
| 10 | `if $i>=$nb goto tailA` | 同上 | FAIL(连带B2，nb错) |
| 11 | `row A $i la` | 1029-1043(row的ci正确) | PASS |
| 12 | `row B $i lb` | 1024 | PASS |
| 13 | `same la lb next` 变量名不展开 | 961-987 | PASS |
| 14 | `echo 11 <$i: $la` | 692-698,1404-1444 | PASS |
| 15 | `echo 12 >$i: $lb` | 同上 | PASS |
| 16 | `:next` | 622-637 | PASS |
| 17 | `set i=$i+1` | 361-387 | PASS |
| 18 | `goto cmp` | 699-711 | PASS |
| 19 | `:tailB` | 标签 | PASS |
| 20 | `if $i>=$nb goto done` | 712-761 | FAIL(连带B2，B长时立即done丢尾行) |
| 21-24 | row B/echo/inc/goto tailB | 同上 | PASS(循环几乎不进入) |
| 25-30 | tailA 段 | 同上 | PASS(na正确) |
| 31-32 | `:done`/`echo 13 ---...` | 692-698 | PASS |

### 3. VK.CSC 核对表

| 行 | 构造 | 实现(源码行) | 结论 |
|---|---|---|---|
| 3 | `load $1`→fa | 989-1008 | PASS |
| 4 | `rows A n` | 1025-1028 | FAIL(连带B2；本例只用fa故n实际正确) |
| 5-6 | maxv=0 / i=0 | 348-401 | PASS |
| 8 | `if $i>=$n goto plot` | 554-578 | PASS |
| 9 | `row A $i line`(行≤79入buf2[80]) | 1029-1043 | PASS |
| 10 | `tok line 1 v` | 921-959 | PASS：**固定按空白切、0基、无自定义分隔符参数**；"label N" 取第1字段(数值)正确 |
| 11-16 | set num/maxv/goto 链 | 348-401,699-761 | PASS |
| 17 | `set a[$i]=$num` 数组写0基 | 663-680,328-346 | PASS（容量64，超限报错截断） |
| 21 | `if $maxv==0 goto done` | 554-578 | PASS：脚本侧除零保护 |
| 22-23 | rows2=10/r=0 | 348-401 | PASS |
| 25 | `set out =` 空串 | 390-400 | PASS |
| 29 | `set val=$a[$c]` 数组读0基 | 413-428,317-326 | PASS |
| 30-31 | `h1=$val*10`、`h=$h1/$maxv` 整型两步比例 | 361-387,373 | PASS：无浮点、整除向零截断、h∈0..10，与10行柱高一致；maxv>0已由行21排除除零 |
| 32-38 | `if $h>$r goto mark`/`out=$out#`/colloop | 554-578,404-439 | PASS（#非alnum，正确追加） |
| 40-45 | echo 11/14/13 | 692-698 | PASS |

### 4. FAIL 明细

**B2【高】`rows <缓冲> <变量>` 解析器 ci 错位，缓冲字母被忽略**（即上文阻塞项 B2）
- 位置：脚本 DIFF.CSC:5-6 / VK.CSC:4；解释器 ocean_tools.c:1011。
- 描述：`u32 ci = (t[1]=='o' && t[2]=='w') ? 3 : 4;`——无论 `row ` 还是 `rows `，t[1]='o'、t[2]='w' 恒成立，ci 恒为3。对 `row A` t[3]是空格可正确跳到 A；但对 `rows A` t[3] 是残留字母 **'s'**，于是 T="s"、N="A"、VAR="na"，缓冲选择 `isb=(T[0]=='B')` 恒假→**永远选 fa**，脚本传的缓冲字母落进 N 被丢弃。
- 后果：`rows A na` 因 A=fa 恰好正确；`rows B nb` 实际取到 fa_n。B 比 A **长**时 tailB 第一行立即 done，**B 多出的尾行被静默丢弃**；B 比 A 短时 row B 越界返空串，A 多出行误配空 B 行打印。
- 修复建议（**改解释器**）：第1011行改为 `u32 ci = (t[3]=='s') ? 4 : 3;`。脚本 `rows A/B var` 写法是文档化正确约定，无需改。

**L6a【低】行数受 128 行 / 8K 双重截断**（261,486-495,1002）：脚本只注释了 ≤8K，未声明 128 行硬上限。建议补注释。

**L6b【低】VK 数组 64 项上限**（267,340）：>64 数据行打印 "array index out of range" 且右侧缺列。建议注释补 "≤64 bars"。

### 5. 通用资源/冲突核对

脚本行数32/46<128、变量~5/~13<32、标签5/10<32、数组1<4、未用 for（goto 循环）、步数远<200000、所有 goto/if-goto/same-goto 目标均有定义、`$1/$2` 位置参数正确、除零有脚本+解释器双保险、内置 TABLE 无 diff/vk 残留、ext_scan 正常注册 diff/vk。全部 PASS。

### 6. 结论

核心语义（双缓冲、row、same、tok 空白切词0基、数组0基、整型两步除法、if 数值比较、echo 拼接、$1..$9、goto/label）**全部吻合**。唯一实质缺陷为 **B2（高）**：解释器 `rows` 关键字跳过长度错误（ocean_tools.c:1011），导致 `rows B` 静默返回 fa 行数，DIFF 在 B 更长时漏打尾部差异行——建议改解释器为 `(t[3]=='s')?4:3`。L6a/L6b 为低 severity 容量提示。
