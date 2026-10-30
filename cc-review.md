# cc-review.md — CatOS 目标一（微型 C 编译器 cc.ce）功能/正确性全量审查

- 审查日期：2026-10-30
- 审查对象：`kernel-v25/extapp/cc.c`（1227 行，微型 C 编译器）、`ce_stub.c`、`build_ce.sh`、`HELLO.C`、`cc_qemu_test.py`；契约侧 `kernel-v25/include/ce.h`、`kernel-v25/src/task.c`（task_spawn_ce）、`kernel-v25/extapp/ce_abi.h`、生产树 `build/mkce.py`/`extapp/ce.ld`
- 审查方式：**双路独立只读审查**（两子代理并行：A=cc.c 核心深度审查；B=构建链/契约/数学独立复核）+ 父代理逐点复核。**未修改任何文件、未运行 QEMU、未重建产物**；仅宿主侧静态分析与数值模拟（Python 验证 rel32/移位公式、既有 cc.ce 头 hexdump 实测）。
- 判定尺度：P0=验收范围失败或编译崩溃；P1=受支持子集产出错误镜像/危险静默失败；P2=陈旧注释/未宣传构造/边缘限制。

---

## 总体判定：**PASS（目标一验收链）**

- **无 P0**。跳板、三处重定位、cdecl 栈帧、while/递归、api-> 内建调用、镜像物理布局、header 字段、内核加载器契约全部正确，与父代理 QEMU 4/4 PASS 互证。
- **7 个 P1**（6 编译器 + 1 验收驱动），全部**不在 HELLO.C 验收路径上**；**8 个 P2**。
- 建议：目标一收口前优先修复 FIND-04（for 中 continue 错编）与 FIND-05（`<<`/`>>` 错编）——二者是"受支持操作符/语句的真实错编"，进目标二前必须修；FIND-01/02/03/06 属静默错编路径，建议同批修复。

---

## A. 跳板正确性 — ✅ 正确（含 1 个 P1 边界）

| 检查点 | 结论 | 证据 |
|---|---|---|
| `g_entry_jmp=e_jmp_fwd()` 在 parse_program 最前 | ✅ | cc.c:922（parse_program 首句；此前 g_olen=0，E9 落在 code offset 0） |
| `e_jmp_fwd` 返回 imm32 字段偏移 | ✅ | cc.c:368 `ob8(0xE9); a=ob_at(); ob32(0); return a;` → a=imm 字段=1（非 E9 本身） |
| `g_entry_off` 在 ce_main 定义处赋值 | ✅ | cc.c:947-948 `name_eqn("ce_main",...) → g_entry_off=ob_at()`，即 ce_main 首指令（push ebp）的 code 偏移 |
| 回填公式 `rel32=g_entry_off-(g_entry_jmp+4)` | ✅ 数值验证 | cc.c:1058-1060；target=(1+4)+rel32=g_entry_off 精确命中 |
| 与 header entry=0 的配合 | ✅ | cc.c:1072 `put32le(&g_hdr[4],0)`；task.c:1343 `h->entry!=0` 拒绝、:1457 `fr->eip=USER_IMAGE_BASE`（=body 偏移 0=跳板） |

**边界缺陷（FIND-01，P1）**：无 ce_main 时 `g_entry_off=0`（cc.c:880 静态初值），rel32=0-(1+4)=**-5**（`E9 FB FF FF FF`，目标=5+(-5)=0=E9 自身）→ **死循环，任务永不返回**。一个无 ce_main 的源文件会"编译成功"却产出挂死镜像，属危险静默失败。
- 一行修复：parse_program 末尾（cc.c:1010 循环后）加 `if (g_entry_off==0) err_set("no ce_main");`
- 注：父代理初始假设的 FC/-4 不精确（imm 字段在 offset 1，故为 FB/-5）；A/B 两路独立推导一致。

## B. fixup_code 三处 reloc 与镜像布局 — ✅ 代码正确；注释陈旧（P2）

镜像物理布局（代码实际）：`20B header | code(g_olen) | data(g_dlen) | 4B api slot`。
证据：build_image cc.c:1067 `img=olen+dlen+4`、:1074 api_off=olen+dlen；save_image :1089/1093/1101/1111 依次写 header/code/data/zero[4]；task.c:1379 把 body[0] 映射到 USER_IMAGE_BASE+0（**无 20B header 位移**）、:1449-1450 把 `VS_TABLE` 写入 `USER_IMAGE_BASE+api`。USER_IMAGE_BASE=0x2000000=CE_LOAD_BASE（paging.h:30）。

| reloc | 公式 | 结论 |
|---|---|---|
| data 绝对地址 | cc.c:1048 `base+g_olen+g_rel_off[i]` | ✅ data body 偏移=code 大小+data 内偏移（记录于 :403） |
| 函数 call rel32 | cc.c:1052 `f->addr-(site+4)` | ✅ site=E8 imm 字段（:528 `ob_at()-4`），f->addr=code 偏移（:951）；数值模拟 target=site+4+rel=f->addr |
| api 槽绝对地址 | cc.c:1044/1057 `base+olen+dlen` | ✅ 与 header api_off=olen+dlen、内核写入地址 USER_IMAGE_BASE+api 三方闭合；`mov eax,[slot]`（A1, :511）→ eax=VS_TABLE → `call [eax+slot*4]` |

**陈旧注释（FIND-07，P2）**：cc.c:1015-1021 注释声称 `[0..3]` 为 api slot、code 在 `[4..]`、"everything is shifted by 4"——与实际代码（slot 在末尾、无 shift）矛盾；cc.c:1182-1183 亦有同类残句但紧随其后自我纠正。代码正确，注释过时，建议删改。

## C. tokenize / parse / codegen 边界

| 检查点 | 结论 | 证据 |
|---|---|---|
| PUNCTS 最长 3 字符匹配 | ✅ | cc.c:223-233 n=3→2→1 命中即 break；`<<=`(3) 先于 `<<`/`=`；`==`(2) 先于 `=`(1)；逐字符推演无误 |
| 字符串转义（gen_postfix 路径） | ✅ | cc.c:479-487 n/t/r/0/\\/"/ 全支持 |
| TK_STR null 结尾（双路径） | ✅ | cc.c:491 与 :981 均 `g_data[g_dlen++]=0`；拷贝以 sizeof(g_data)-1 为界留 NUL |
| 递归 fib 栈帧 / cdecl | ✅ | parse_func :886-887 重置 g_nloc/g_frame；参数 off=8+np*4（:897）；prologue :907-909 push ebp/mov ebp,esp/sub esp,(g_frame+15)&~15；epilogue mov esp,ebp/pop ebp/ret（:913）；调用方 e_add_esp(n*4) 清栈 |
| for 的 step 布局（不含 continue） | ✅ | cc.c:840-856 top/cond/jz end/jmp body/body/jmp step/step/jmp top 布局正确 |
| 数组越界无检查 | ✅ by design | gen_addr_indexed :454-464 无边界检查——tiny C 正常行为，越界仅影响用户态自身数据/触发缺页杀任务，不越内核 |
| &&/|| 短路 + 0/1 归一 | ✅ | cc.c:582-591（AND：left==0 跳 false 不求值 right）、:593-602（OR 对称）；结果 setcc+movzx 归 0/1 |

**缺陷 FIND-02（P1）**：全局字符串初始化转义不全。parse_program 路径（cc.c:973-976）switch 仅 `\n`/`\t`，default 原样复制 → **`\0`→'0'(0x30)、`\r`→'r'(0x72)**；与 gen_postfix 路径（479-487 全转义）不一致。全局 `char *s="a\0b"` 字节错误。修复：default 前补 `case 'r': c='\r'; break; case '0': c=0; break;`。

**缺陷 FIND-03（P1）**：全局 char 数组字符串初始化按指针处理。`char msg[8]="str"`（is_arr=1）时，cc.c:966-985 仍把串追加到 g_dlen 并在数组槽写 4 字节偏移指针（不内联拷入数组区）→ 运行时 `msg` 内容为"偏移整数小端 + 0"垃圾。修复：is_arr 时把串字节内联拷入 g->addr 槽位（strncpy 式）。

**缺陷 FIND-04（P1）**：**for 的 continue 跳转从不回填**。for 处理器 cc.c:854 只回填 g_brk（break→end），:856 `g_ncont=cb` 直接把 continue 记录丢弃；对比 while 处理器 :823 明确回填 `top-g_cont[i]-4`。后果：continue 发出的 `E9 00000000` 永不补全 → 运行时跳到 continue 之后的下一条 body 语句，**step 子句被跳过、语义错误**（仅当 continue 恰为 body 末句时偶然正确）。修复：:856 前补 `for (i=cb;i<g_ncont;i++) ob_patch(g_cont[i], step_pos-g_cont[i]-4);`（for 的 continue 应跳 step_pos，非 while 的 top）。

**缺陷 FIND-05（P1）**：**`<<`/`>>` 移位操作数错位**。cc.c:618-619 用 `e_mov_eax_ecx()`（mov eax,ecx）后 `shl/sar eax,cl`——pop 后约定 ecx=left、eax=right，但 mov 后 **ecx 仍=left，cl=left 低字节** → 实际计算 `left << (left&31)`，**右操作数被丢弃**（3<<2 实得 24，非 12）。对照除法 :612 正确用 `xchg eax,ecx`。修复：:618/619 改 `ob8(0x87);ob8(0xC8);`（xchg，与 :612 一致）。

**缺陷 FIND-06（P1）**：重定位/调用/api 引用容量超限静默丢弃。REL_MAX=128（:403）、CALLMAX=128（:527-531）、APIREF_MAX=64（:512）满容后**已发出的 0 立即数永不回填** → mov eax,0 / call +0 / mov eax,[0]，无任何报错。修复：满容分支 `err_set("reloc table full")`。

**P2 项**：FIND-08 ob8 超 cap 静默丢字节（:83）且 heap 分配失败回退 8K 静态缓冲时大镜像被静默截断（:72-76）；FIND-09 `unsigned` 被 parse_type 接受（:739）但 codegen 全程按有符号（idiv/sar/有符号 setcc），建议 err_set 拒绝；FIND-10 参数先经 local_new 计入 g_frame 再覆盖 off（:896-897）→ 每参数多 4B 无效栈分配，无害。

## D. save_image 分块写 — ✅ 正确

- :1089 header 20B 以 mode=0（截断）写，:1090 起 mode=1（追加）——符合 ce.h file_write 语义；
- :1093-1098 / :1101-1106 code/data 均按 16K 分块（:1095/:1103 `n>16384` 截断），满足 ce.h file_write 单次 ≤16K 契约；
- :1109-1113 写 4B 全 0 api slot（内核加载时覆写）；
- :1115 `total != g_imglen` 校验：total=20+olen+dlen+4，g_imglen=20+(olen+dlen+4)（:1068），自洽；
- build_image :1067-1075：img=olen+dlen+4、api_off=olen+dlen → 内核 :1345 `api+4>img` 以等号放行，边界正确；IMG_MAX=256K（:1023）与内核 CE_MAX_IMAGE=0x40000 一致。

## E. build_ce.sh 各修复点 — ✅ 正确

- **-fno-stack-check + __chkstk_ms 空桩**：✅。cc.c 无超页局部数组（大缓冲为静态/堆，见 :68-140），触发源是 MinGW 对超一页栈帧的探测调用；ce_stub.c 空桩（EAX=帧大小、ECX=旧 SP，不破坏调用者寄存器）满足 ABI。禁检查+空桩双保险成立。
- **ce_abi.h 48 槽对齐**：✅。逐槽数出 ce_abi.h 与 kernel-v25/include/ce.h 均 **48 成员**（槽0 write … 槽33 getuid … 槽37 hw_num … 槽47 wait），顺序/类型逐行一致；cc.c `API_NAMES[]`（:327-338）与内核 nargs/taddr 表（task.c:498-512）三方同序，**无错位**。
- **.bss 剥离与 bss_size**：✅。实测 cc.ce 头：magic=CE01、entry=0、bss=0xB3C4=46020B、api=23244、image=23248B——image+bss 远低于 USER_VSYSCALL=0x2080000；`objcopy -R .bss` 后再 `-O binary`（避免约 851KiB 零填充超 CE_MAX_IMAGE）与内核清零 bss 的契约一致。
- **--disable-reloc-section / ce.ld / nm 检查 / API_HEX**：✅。ce_main VMA=0x2000000（.text.ce_entry 置首）、nm 检查 `02000000`、`VMA−0x2000000` 换算（python 兼容 dash/MinGW-sh）均自洽。

**P2 项**：
- **FIND-02(E) mkce.py:31 硬编码 64K（0x10000）旧上限**：生产树 build/mkce.py 仍以 0x10000 拒绝镜像，而 ce.h 的 CE_MAX_IMAGE 已升至 256K。cc.ce 自身仅 23KB 不受影响，但工具链无法包装 >64K 的程序镜像。修复：改为按 CE_MAX_IMAGE（或传入参数）判定。
- **FIND-05(E) kernel-v25/ 树缺 ce.ld 与 build/mkce.py**：`ls` 证实 kernel-v25/build/ 与 kernel-v25/extapp/ce.ld 不存在，build_ce.sh 单独无法在 kernel-v25 树内重建（依赖位于 Catos_Better_better 生产树，cc.ce 字节一致）。属构建树快照缺口，建议补入或文档注明。

## 内核加载器契约逐条核对 — ✅ 全部通过

尺寸/魔数/`entry==0`/img≤256K/bss≤256K/img+20≤文件大小/`api+4≤img`（task.c:1331-1345）全部正确；body 直载 `USER_IMAGE_BASE(0x2000000)+body偏移`（:1366/:1379）、首簇跳 20B 头、跨簇走 FAT 正确；**api 槽写入 `USER_IMAGE_BASE+api_offset`、值为 VS_TABLE**（task.c:1449-1450，VS_TABLE=VSYSCALL_BASE+0x800=0x2080800）——与 cc.c 槽地址公式、header api_offset 三方闭合；bss 清零（kzalloc_frame）、64KiB 用户栈、`fr->eip=USER_IMAGE_BASE` 恒 0 入口全部正确。
- nargs[35]=2 对应 kill(int tid)（1 参）多读一个参数、handler 不使用——**无害，P2**（task.c:507）。

## HELLO.C 覆盖度与 cc_qemu_test.py — 功能覆盖可，断言有假阳性

- HELLO.C 覆盖递归 fib、while 求和、api->write/print_int 调用——目标一关键路径齐全；缺负数 print_int 用例（P2）。
- **FIND-03(测试) check() OR 语义假阳性（P1）**：cc_qemu_test.py:181-186 `for nd in needles: if nd in scr: return True` —— 任一 needle 命中即 PASS。:215 三个 needle（'cc-compiled hello'、'fib(10)=55'、'sum(5)=15'）命中其一即判过 → **fib/sum 数值打错（如 fib(10)=99）仍会判通过**，数值正确性未被真断言。修复：改为全部 needle 必中（AND 语义）或对数值行做正则精确匹配。注：不影响"实测 4/4 PASS 为真"的事实（真实屏幕内容正确），只影响未来回归的断言强度。
- 其余断言（'OK -> /BIN/OUT.CE' 编译成功、重启后重编 OUT2.CE）无假阳性风险。

---

## F. 双审判定汇总

| 级别 | 数量 | 内容 |
|---|---|---|
| **P0 阻断** | 0 | — |
| **P1 建议** | 7 | FIND-01 无 ce_main 死循环；FIND-02 全局串转义 \0/\r；FIND-03 全局 char 数组串初始化垃圾；FIND-04 **for continue 不回填**；FIND-05 **`<<`/`>>` 错用左操作数作移位计数**；FIND-06 reloc/call/apiref 满容静默丢弃；FIND-03(测试) check() OR 假阳性 |
| **P2 可选** | 8 | 陈旧注释（cc.c:1015-1021、:1182-1183）；ob8 截断/heap 回退；unsigned 按有符号；参数帧浪费；mkce.py 64K 旧上限；kernel-v25 缺 ce.ld/mkce.py；nargs[35]=2；HELLO.C 缺负数用例 |

**结论**：
1. **不会导致 HELLO.C 引导链失败或编译崩溃——目标一 PASS，无 P0。**
2. 存在 6 处会导致**受支持子集产出错误镜像**的编译器缺陷（P1），其中 FIND-04/05 是对受支持语句/操作符的直接错编，**进目标二前必须修复**；FIND-01/02/03/06 为静默错编路径，建议同批修复。
3. 验收驱动 check() 的 OR 语义使数值断言偏弱（P1），建议强化后再作长期回归门槛。

## 方法声明

本次审查为**双路独立只读审查**（cc.c 核心审查 × 构建链/契约/数学复核，两路对跳板、reloc、无 ce_main 边界结论完全一致）加父代理逐点复核（task.c api 槽写入、nargs 表、mkce.py 上限、kernel-v25 缺文件均亲验）。**未修改任何文件、未运行 QEMU、未重建任何产物**；数值结论来自宿主侧 Python 模拟与既有 cc.ce 头 hexdump 实测，父代理 QEMU 4/4 PASS 作为外部事实采信。
