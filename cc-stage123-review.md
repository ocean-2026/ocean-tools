# cc-stage123-review.md — CatOS cc.c 自举环阶段1-3（struct/typedef/enum + switch + goto/++--）功能/正确性独立审查

- 审查日期：2026-10-31
- 审查对象：`kernel-v25/extapp/cc.c`（1776 行，md5 `87fbf4f7f763e011bed26ebe1aae44b226`，与 `Catos_Better_better/Catos_Better_better/extapp/cc.c` 双副本 md5 一致）；本地 commit `430a142`；工作区 5 个回归用例 HELLO.C / EXTRA.C / STRUCT.C / SWITCH.C / GOTO.C；宿主桩 `cc-dev/host_stub.c`；开发子代理自述 `history.md` 第 52-55 行三条 [CatOS-cc编译器] 易错点。
- 审查方式：**只读全量审查 + 宿主侧独立复验**。未修改 cc.c、未重建 cc.ce、未运行 QEMU、未做任何 git 操作。在 `/tmp/cc_review/` 沙箱中用 `gcc -O0 -w -I. -o cc_host cc.c host_stub.c` 编译宿主二进制，对 5 个用例独立编译、hexdump CE01 头、objdump 反汇编逐条核对 codegen；并额外构造 `CHARBUG.C` 定向复现一处 latent 错编。
- 判定尺度：P0=验收链失败/崩溃/错编导致用例断言失败；P1=受支持子集产出错误镜像或危险静默失败（不在本次 5 用例断言路径上但会在相似代码上错编）；P2=健壮性/边缘限制/陈旧注释/死代码。

---

## 总体判定：**PASS（阶段1-3 验收链）— 无 P0，但发现 1 个 P1 latent 错编**

- **5 个回归用例宿主侧全部编译成功**，CE01 头 20 字节契约逐条合规（magic=`CE01`、entry=0、bss=0、api_off+4=image、文件长度=20+image），无任何用例触发编译器报错。
- **反汇编核对关键 codegen 全部正确**：struct 成员偏移（x@0 / name[12]@4 / y@16 / size=20）、typedef 别名 sizeof=20、enum A=0 B=1 C=10 D=11、switch 三处 jne 回填到下一 case 比较点、break 全部跳到 switch 收尾 `add esp,4`、goto 前向/后向跳转 rel32 数值逐字节闭合、前缀 ++ 返回新值、后缀 ++ 返回旧值。
- **1 个 P1 latent 错编（FIND-S1-01）**：struct 内 char 数组成员经 `[idx]` 下标赋值时，store 尺寸被错设为整个 struct 大小（20 字节），最终落到 4 字节 `mov [eax],ecx` 而非 1 字节 `mov [eax],cl`。**在 STRUCT.C 中不爆发**（name[12] 占 12 字节、相邻 int y 在 offset 16，4 字节 store 越界最远到 offset 9，碰不到 y），但在 `char buf[4]; int x;` 这种紧排布局下会把相邻 int 成员清 0（CHARBUG.C 定向复现：`r.x=42` 被后续 `r.buf[1..3]=...` 四字节 store 覆盖成 0）。
- **开发子代理 history.md 三条自述全部属实**：switch jne 回填位置、lvalue 地址模型重构、host_stub 闭环经验——经反汇编逐条验证，无夸大、无隐瞒。

---

## A. 阶段1 struct / typedef / enum — ✅ 主体正确；1 个 P1 latent

### A.1 struct 定义表与成员偏移 — ✅

| 检查点 | 结论 | 证据（cc.c 行号 / 反汇编） |
|---|---|---|
| `parse_struct_def` 成员对齐 | ✅ | cc.c:1353-1354 `align = mptr?4:msz; if (align!=1) off=(off+align-1)&~(align-1)`；char 成员对齐 1、int 对齐 4、指针对齐 4 |
| STRUCT.C point_t 布局 | ✅ 数值正确 | int x@off=0..3 / char name[12]@off=4..15 / int y@off=16..19 / size=(20+3)&~3=20 |
| 反汇编 `p.y = p.x*2` | ✅ | STRUCT.body `2000021 lea -0x1c(%ebp),%eax; add $0x10,%eax` → &p.y=ebp-12；imul 出 10；`mov %ecx,(%eax)` 4 字节 store |
| 反汇编 `pp->x = 7` | ✅ | `20000c7 lea -0x20(%ebp),%eax; mov (%eax),%eax`（解 pp 指针）；moff=0 故无 `add`；4 字节 store 7 到 p.x |
| 反汇编 `gp.x = p.x + pp->y` | ✅ | `20000dc mov $0x20002a8,%eax`（gp 全局，data 基=0x2000000+code680）；`lea -0x20(%ebp),%eax; mov (%eax),%eax; add $0x10,%eax; mov (%eax),%eax` → pp->y=10；add ecx(7) → 17；store 到 gp.x |
| `sizeof(point_t)` | ✅ | `200026e mov $0x14,%eax`（0x14=20）；parse_type 走 alias 分支取 `g_st[stidx].size` |
| `typedef struct {...} point_t;` | ✅ | parse_typedef :1376 调 parse_struct_def 后 add_alias(stidx=struct 索引)；parse_type :1022 命中 alias 并回填 sz/stidx |
| enum `{A,B,C=10,D}` | ✅ | 反汇编 print_int 立即数依次 `mov $0x0/$0x1/$0xa/$0xb` = 0/1/10/11；parse_enum :1400 起 v 自增 |
| `&p` 取址 | ✅ | GOTO/STRUCT 反汇编 `lea -0x1c(%ebp),%eax` 直接给地址；gen_unary OP_AMP :755 改调 `gen_lvalue(as,0)`（do_load=0）不再误取值 |
| 全局 struct 分配（gp） | ✅ | parse_program :1484-1489 按 sz=20 零填 20 字节；data 段 hexdump 前 20 字节全 0，后接字符串字面量 |

### A.2 P1 错编：struct 内 char 数组成员 `[idx]` store 尺寸错误（FIND-S1-01）

**根因**：`gen_lvalue` 在 `[idx]` 下标分支末尾（cc.c:608-610）按**原始符号 `s`** 重置 obj_st/obj_sz/esize，而不是保留刚访问的数组成员元素尺寸：

```c
if (s->is_ptr)          { obj_ptr=1; obj_st=s->stidx; obj_sz=4;       esize=4; }
else if (s->stidx >= 0) { obj_ptr=0; obj_st=s->stidx; obj_sz=s->sz;   esize=s->sz; }
else                     { obj_ptr=0; obj_st=-1;      obj_sz=s->sz;   esize=s->sz; }
obj_arr = 0;
```

对 `p.name[0] = 'h'`（p 是 point_t，s->stidx≥0，s->sz=20）：进入 `[0]` 前 esize=1（char 元素），imul 用 1 缩放地址正确；但下标结束后 obj_sz 被重置为 s->sz=20，`g_lv_sz=20`。store 分支（gen_assign_expr :892 / do_incdec :980）判 `if (es==1) st8 else st4`，es=20≠1 → 走 4 字节 `mov %ecx,(%eax)`（89 08），**不是 1 字节 `mov %cl,(%eax)`（88 08）**。

**反汇编实证**（STRUCT.body）：
```
2000047: lea -0x1c(%ebp),%eax      ; &p
200004d: add $0x4,%eax            ; &p.name
2000054: mov $0x0,%eax            ; idx=0
200005a: add %eax,%ecx            ; &p.name[0]
200005f: mov $0x68,%eax           ; 'h'
2000067: mov %ecx,(%eax)          ; *** 4 字节 store，应为 88 08 ***
```
后续 `p.name[1]='i'`、`p.name[2]=0` 同样是 89 08 四字节 store。STRUCT.C 最终 name 内容为 `['h','i',0,0,0,...]`，因 name[12] 长达 12 字节、相邻 y 在 offset 16，四字节 store 越界最远到 offset 9，碰不到 y；且 name[2] 恰好被置 0 作 NUL 终止，`api->write(p.name)` 输出 "hi" 正确。**用例过，但 codegen 是错的。**

**定向复现（CHARBUG.C，宿主侧独立构造）**：
```c
typedef struct { char buf[4]; int x; } rec_t;
rec_t r;
r.x = 42;
r.buf[0] = 'A'; r.buf[1] = 'B'; r.buf[2] = 'C'; r.buf[3] = 'D';
// 期望 x=42, buf="ABCD"
```
布局：buf@0..3、x@4..7。反汇编确认每条 `r.buf[k]=...` 都是 89 08 四字节 store：
- `r.buf[1]='B'` store 在 offset 1..4 → **覆写 x 的低字节为 0**
- `r.buf[2]='C'` store 在 offset 2..5 → 覆写 x 低 2 字节
- `r.buf[3]='D'` store 在 offset 3..6 → 覆写 x 低 3 字节
- 最终 r.x=0（应为 42）；buf="ABCD" 虽对，但 x 已毁。

**一行级修复建议**（cc.c:608-610）：下标分支的状态重置应基于**刚访问的成员**而非原始符号。最小改动：在 `[idx]` 分支开头把当前 obj_sz/obj_st/obj_ptr/obj_arr/esize 保存为"进入下标前的成员状态"，下标结束后**按该状态的元素类型**恢复（数组→元素尺寸；指针→指针；结构→结构），而不是回退到 `s->sz`。或者更简单：在 `[idx]` 分支末尾不要重置 obj_st/obj_sz，仅把 obj_arr 清 0 并保持 esize（元素尺寸）透传，让 g_lv_sz=esize（对 char 数组 esize=1，对 int 数组 esize=4，均正确）。

**影响范围**：仅"struct 内 char 数组成员经 `[idx]` 单字节赋值"一种模式触发；纯局部 `char buf[10]`（s->stidx=-1）走 else 分支 obj_sz=s->sz=1，正确；struct 内 int 数组成员 esize=4 与四字节 store 一致，碰巧正确；单 char 成员（无 [idx]）g_lv_sz=1，正确。

---

## B. 阶段2 switch / case / default — ✅ 用例正确；2 个 P2 语义偏差

### B.1 反汇编核对（SWITCH.body）

```
2000021: lea 0x8(%ebp),%eax; mov (%eax),%eax   ; n = arg
2000029: push %eax                              ; 保存 switch 值
200002a: mov (%esp),%eax; cmp $0x1,%eax         ; case 1 比较
2000032: jne 0x2000050                          ; ≠1 → case 2
2000038: ... r=100(0x64) ...; jmp 0x20000b6     ; break → end
2000050: mov (%esp),%eax; cmp $0x2,%eax; jne 0x2000076
200005e: ... r=200(0xc8) ...; jmp 0x20000b6
2000076: mov (%esp),%eax; cmp $0x5,%eax; jne 0x200009c
2000084: ... r=500(0x1f4) ...; jmp 0x20000b6
200009c: ... r=-1 (mov $1; neg) ...; jmp 0x20000b6
20000b6: add $0x4,%esp                          ; pop switch 值
```

- 三处 jne 回填目标分别为下一 case 的 `mov (%esp),%eax` 比较点（0x2000050 / 0x2000076 / 0x200009c），**不是** break 的 jmp 位点——与 history.md 第 53 行自述"jne 必须在 case 体解析完后回填"完全一致。
- 四个 break 的 jmp 全部回填到 0x20000b6（switch 收尾）。
- `default: r=-1` 用 `mov $1; neg` 生成 -1，cmp 立即数无对应比较（default 是 fallback）。
- switch 值用 `push %eax` 存栈，case 比较前 `mov (%esp),%eax` 重读，结束 `add $0x4` 弹栈——栈平衡正确。

### B.2 P2 语义偏差（不在用例路径上）

- **B-P2-1：fallthrough 并非"每 case 自动终止"**。history.md/任务描述称"每 case 自动终止"，但代码（cc.c:1198-1209）是：case 体的内层 while 循环以 `break` / 下一 label / `}` 为终止符，**没有自动插入 break**。若用户漏写 break，执行会落入下一 case 的 `cmp eax,nextv; jne` 比较点——这不是标准 C 的穿透语义（不会无条件跑下一 case 体），也不是自动终止，而是"再比一次"。对 SWITCH.C（每 case 都有 break）无影响，但建议文档如实描述："不支持穿透，漏写 break 会落到下一 case 比较点（通常等于 default）"。
- **B-P2-2：default 前置会先于所有 case 执行**。default 分支（cc.c:1168-1181）没有任何比较守卫，若 `default:` 写在 case 之前，它的语句会**无条件发射**在 case 比较链之前，导致无论 switch 值如何都先跑 default 体。SWITCH.C 中 default 是最后一个 case，无影响；建议 parse 时检测 default 位置并 warn，或在 codegen 中把 default 体 emit 到比较链末尾。

---

## C. 阶段3 goto / 前后置 ++/-- — ✅ 全部正确

### C.1 goto 前向/后向跳转（GOTO.body）

```
2000034: loop:                          ; g_lbl_off[loop]=0x2000034
200003c: ... cmp i,5; setge; test ...
200004d: je  0x2000058                  ; i<5 → 跳过 goto done
2000053: jmp 0x200009e                  ; goto done（前向，回填后指向 done）
2000058: s=s+i; i++; ...
2000099: jmp 0x2000034                  ; goto loop（后向 rel32=-106，数值闭合）
200009e: done:                          ; g_lbl_off[done]=0x200009e，回填前向 goto
```

- 前向 goto（`if (i>=5) goto done;`）：goto 发射时 done 未定义，记 `g_goto_at[]`；done label 定义时（cc.c:1251-1256）遍历回填 `ob_patch`，rel32=ob_at-g_goto_at-4，数值闭合（0x2000058+0x46=0x200009e）。
- 后向 goto（`goto loop;`）：loop label 已定义，`e_jmp_back(g_lbl_off[idx])` 直接发 E9 rel32=-106，目标=0x200009e-106=0x2000034。✓

### C.2 后缀 i++（GOTO.body 200011b-2000143）

```
200011b: lea -0xc(%ebp),%eax       ; &i
2000121: push %eax
2000122: mov (%esp),%eax           ; =addr
2000125: mov (%eax),%eax           ; old = *&i
2000127: push %eax
2000128: add $0x1,%eax             ; new = old+1
200012b: mov 0x4(%esp),%ecx        ; ecx = addr
200012f: mov %eax,(%ecx)           ; *&i = new
2000131: mov (%esp),%eax           ; *** 返回 old（后缀语义）***
2000134: add $0x8,%esp
200013a: push %eax                  ; 旧值 3 作 print_int 参数
```

### C.3 前缀 ++i（GOTO.body 2000188-20001ad）

```
2000188: lea -0xc(%ebp),%eax       ; &i
200018e: push %eax
200018f: mov (%esp),%eax; mov (%eax),%eax  ; old
2000194: push %eax
2000195: add $0x1,%eax             ; new
2000198: mov 0x4(%esp),%ecx; mov %eax,(%ecx)  ; *&i=new
                                  ; *** 不再 mov (%esp),%eax（前缀返回新值）***
200019e: add $0x8,%esp
20001a4: push %eax                  ; 新值 5 作 print_int 参数
```

do_incdec（cc.c:971-988）prefix=0 时末尾 `mov (%esp),%eax`（取旧值），prefix=1 时跳过（eax 已是新值）。前后值语义正确。GOTO.C 预期 `post=3 i2=4 pre=5` 与 codegen 一致。

---

## D. 回归无倒退 — ✅ HELLO.C / EXTRA.C

| 用例 | 编译结果 | 关键 codegen | 结论 |
|---|---|---|---|
| HELLO.C | 141 token / code 479B / image 542B | fib 递归 call rel32 两次均指向 0x2000005；`cmp $2,n; setl` 正确；while 求和循环结构完整 | ✅ 无倒退 |
| EXTRA.C | 239 token / code 779B / image 853B | `3<<2` 用 `xchg %ecx,%eax; shl %cl,%eax`（FIND-05 修复在位）；for continue 回填 step_pos（FIND-04 修复在位）；`g_msg[8]="hi"` data 段内联 `hi\0\0\0\0\0\0`；`g_p` 指针槽值=0x2000317（绝对地址，FIND-02/DREL 修复在位） | ✅ 无倒退 |

---

## E. CE01 契约 — ✅ 5/5 合规

宿主侧 hexdump 逐字段核对：

| 用例 | magic | entry | bss | api_off | image | api_off+4≤image | 文件长度=20+image |
|---|---|---|---|---|---|---|---|
| HELLO.CE | CE01 | 0 | 0 | 518 | 522 | 522≤522 ✓ | 542=20+522 ✓ |
| EXTRA.CE | CE01 | 0 | 0 | 829 | 833 | 833≤833 ✓ | 853=20+833 ✓ |
| STRUCT.CE | CE01 | 0 | 0 | 740 | 744 | 744≤744 ✓ | 764=20+744 ✓ |
| SWITCH.CE | CE01 | 0 | 0 | 485 | 489 | 489≤489 ✓ | 509=20+489 ✓ |
| GOTO.CE | CE01 | 0 | 0 | 500 | 504 | 504≤504 ✓ | 524=20+504 ✓ |

- entry 跳板 `E9 rel32` 在 body offset 0，header entry 恒 0（task.c 要求）。
- api_off = code+data，api slot 在 body 末尾 4 字节，`api_off+4=image` 边界刚好等于（task.c:1345 `api+4>img` 拒绝，等号放行）。
- 未修改任何头文件（ce_abi.h 48 槽与 cc.c API_NAMES 一一对应，未动）。

---

## F. 开发子代理自述验证 — ✅ 三条全属实

| history.md 自述（第 52-55 行） | 本次独立验证 |
|---|---|
| switch jne 必须在 case 体解析完后回填，否则回填到 break E9 上 | ✅ SWITCH.body 三处 jne 分别指向 0x2000050/0x2000076/0x200009c（下一 case 比较点），不是 0x20000b6（end）；cc.c:1198-1209 内层 while 吃完 case 体再 `e_patch_here(jne)` |
| 成员访问必须走 lvalue 地址模型，`&x` 旧实现实为取值 | ✅ gen_lvalue :581 先 lea/e_data_addr 取基址再链 . / -> / [idx]；gen_unary OP_AMP :755 调 `gen_lvalue(as,0)` 返回地址；STRUCT.body `pp=&p` 反汇编为 `lea -0x20(%ebp),%eax`（赋值 pp 为 &p），非取值 |
| host_stub 闭环 + objdump 反汇编可秒级定位 codegen | ✅ 本次复用 host_stub.c 编译 cc_host，5 用例宿主侧编译全过，objdump 逐条核对偏移/jne/rel32 数值闭合 |

无夸大、无隐瞒。但 history.md 未记录 FIND-S1-01（struct char 数组下标 store 尺寸错）——这是审查新发现，建议补登。

---

## G. P0/P1/P2 汇总

| 级别 | 数量 | 内容 |
|---|---|---|
| **P0 阻断** | 0 | 5 用例宿主侧全编译过、CE01 头合规、关键 codegen 数值闭合；无崩溃、无用例断言失败 |
| **P1 建议** | 1 | **FIND-S1-01**：struct 内 char 数组成员经 `[idx]` 赋值时 g_lv_sz 被错设为 struct size（20），store 走 4 字节而非 1 字节；CHARBUG.C 复现相邻 int 成员被清 0。STRUCT.C 因 name[12] 长、y 在 offset 16 而未爆发。修复见 A.2 |
| **P2 可选** | 6 | ① switch fallthrough 文档与代码不符（不自动终止也不标准穿透，见 B-P2-1）；② default 前置会无条件先执行（B-P2-2）；③ 状态行 `api->print_int(0)` 把 "0" 拼到 data 尺寸后，显示 "data 600 B" 实为 60 B（cc.c:1763）；④ `gen_addr_indexed`（cc.c:539）为死代码，重构后无人调用；⑤ 参数先经 local_new 计入 g_frame 再覆盖 off（cc.c:1286-1287，FIND-10 遗留，每参数多 4B 栈帧）；⑥ REL_MAX/CALLMAX/APIREF_MAX 满容静默丢弃（FIND-06 遗留，本轮未引入但仍在位） |

---

## H. 宿主复验逐用例结果

| 用例 | 宿主编译 | token 数 | code/data/imgae | 反汇编关键核对 | 预期输出 |
|---|---|---|---|---|---|
| HELLO.C | OK | 141 | 479/390/542 | fib 递归 call 正确、setl 比较正确、while 循环结构正确 | `cc-compiled hello\nfib(10)=55 sum(5)=15\n`（与历史一致） |
| EXTRA.C | OK | 239 | 779/500/853 | `xchg;shl` 在、for continue 回填 step 在、g_msg 内联 hi\0、g_p 绝对地址 0x2000317 | `sh=12 t=8 c=3 z=12 msg=hi p1=A p2=B` |
| STRUCT.C | OK | 267 | 680/60/764 | x@0/name@4/y@16、sizeof=20(0x14)、enum 0/1/10/11、gp 全局 20 字节零填 | `x=7 y=10 gp=17 nm=hi A=0 B=1 C=10 D=11 sz=20`（**输出值对，但 name 写入 codegen 有 P1 latent**） |
| SWITCH.C | OK | 151 | 464/210/509 | jne 三处回填到下一 case 比较点、break 全到 end、default -1 用 neg 生成 | `c1=100 c2=200 c5=500 c9=-1` |
| GOTO.C | OK | 137 | 473/270/524 | goto 后向 rel=-106 到 loop、前向回填到 done、后缀返回旧值/前缀返回新值 | `s=10 i=5 post=3 i2=4 pre=5` |

---

## I. 与开发子代理宣称不符的点

1. **"每 case 自动终止"表述不精确**：代码未自动插入 break，漏写 break 会落入下一 case 比较点（B-P2-1）。不影响 SWITCH.C 但文档应修正。
2. **history.md 未登记 FIND-S1-01**：struct char 数组下标 store 尺寸错是本轮新发现，开发子代理的"lvalue 地址模型重构"自述只说了地址链正确，未提 g_lv_sz 在 [idx] 后被错重置这一边界。建议补登 history.md。
3. **状态行 "data 600 B" 显示误导**：实际 data=60 字节，多余的 "0" 来自 cc.c:1763 `api->print_int(0)`（疑似遗留调试代码）。不影响功能但会误导读日志的人。

---

## 方法声明

本次审查**只读**：未修改 cc.c 或任何被测文件、未重建 cc.ce、未运行 QEMU、未做 git 操作。在 `/tmp/cc_review/` 沙箱中独立编译 `cc.c + host_stub.c` 为宿主二进制 `cc_host`（gcc -O0 -w，64 位 Linux），对 5 个回归用例独立编译并 hexdump CE01 头；用 `objdump -b binary -m i386 --adjust-vma=0x2000000 -D` 反汇编 body 逐字节核对成员偏移、jne 回填、goto rel32、前后缀 ++ 栈布局；额外构造 `CHARBUG.C` 定向复现 FIND-S1-01。QEMU 10/10 PASS（开发子代理宣称、cc_shots/ 截图可采信）作为外部事实，本次以宿主侧 codegen 反汇编为复验依据。
