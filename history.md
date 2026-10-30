# history.md - 易错点记录

格式：[所属项目][易错点]内容
判定准则：只要是因为管理不当或非技术原因导致的、细心就能避免的意外，就算易错点（2026-10-24 用户定稿）。

- [ot][易错点] execve 加载 CE 时 image 必须**跳过 16 字节 CE01 头**从 fb+16 拷入 0x02000000；若连头一起拷，0x02000000 处是 'CE01' 魔数数据，ce_main 实际在 +16 处，call 0x02000000 执行垃圾指令导致输出乱码（v1.9 乱码根因，已修）
- [ot][易错点] QEMU 新启动实例的 PS/2 键盘注入可能**完全失效**（HMP sendkey / QMP input-send-event / 长 hold 均 0% 成功率，实测 12+ 新实例全丢），仅长时间运行的老实例部分正常；VM 交互测试前先发单键探测键盘，不可假设键盘可用
- [ot][易错点] `pkill -f qemu-system` 会匹配 bash -c 包装进程的命令行导致**误杀自身 shell**（exit 137）；杀 QEMU 用 pidfile 或 `ps aux | grep "[q]emu" | awk '{print $2}' | xargs -r kill -9`
- [ot][易错点] exec_launch 用 i386 固定寄存器约束（S/D/c/a/d/b 共 6 个）触发 GCC "asm operand has impossible constraints"（32 位仅 8 个寄存器）；参数改走栈（"m" 内存操作数 push + push 立即数 + ret 跳转）
- [ot][易错点] QEMU monitor 是行式交互，新连接会先收到欢迎横幅（"type 'help' for more information"），脚本解析响应时勿把横幅当命令回显
- [ot][易错点] vga.py 的 q_cmd 超时 1s 会截断 xp 大输出（2000 字 dump 需 ~10s），monitor 命令积压拖垮后续 sendkey；读屏超时应 ≥10s
- [ot][易错点] v1.9 exec 跳板需完全位置无关（拷贝到固定区 0x0147c800 执行），跳板内引用外部符号地址（如 g_csc_fb）会导致跳板内容含绝对地址失效；用寄存器传递/自我计算替代
- [CatOS双线开发][易错点] 多项目并行共享同一 QEMU 环境时，用 `pkill -f qemu` / `pkill qemu` 全量清理会**连坐杀掉另一项目正在运行的 VM**（ot 链周期清理时游戏链 VM 被误杀 3 次）；正确做法：① 各项目用独立磁盘副本（adofai/disk-test.img）+ 独立 monitor socket（catos-mon.sock vs catos-mon2.sock）；② 清理只按自身 pidfile 精确 kill 或按 socket 文件归属匹配，禁止全杀；③ 共用 disk.img 时改盘前先确认对方未在用
- [ot][易错点] C 函数参数名遮蔽全局函数：my_readline(char *out, ...) 的参数 out 遮蔽了全局 out() 输出函数，函数体内调用 out() 报 "called object 'out' is not a function or function pointer"；参数名避免与全局函数重名，或改用 outln/out_int/直接 api->write0
- [ot][易错点] QEMU sendkey 长 hold 参数（sendkey o 100）会触发按键重复风暴，对已处于 #34 键盘失效的实例产生大量乱码注入（屏幕出现 0x13a14511 类内存 dump 乱码），实例即废；键盘失效时勿再用 hold 强化重试
- [ot][易错点] **`gcc ... | head -N` 管道会吞掉错误信息，且 `$?` 取的是 head 的退出码不是 gcc 的**：编译失败时旧 .o 文件保留，ld 仍用旧对象产出"新"产物 → 改码后产物不变（本轮回放：ACL 删除后 ocntools.ce 仍含 su/whoami 就是此坑）。判编译成败用 `gcc ...; echo $?`（无管道）或 `&&` 链；改码后必须核对产物内容/时间戳（strings/grep 产物关键字）
- [ot][易错点] qemu-img convert 在无 flock 支持的文件系统（hpvs_fs /home/user）报 "Failed to get resize/write lock"：目标路径换到 /tmp（overlay 支持锁），或 QEMU 磁盘镜像放 /tmp 再启动
- [ot][易错点] FAT32 目录项是 11 字节 8.3 名（无点）：注入脚本匹配名字要 `b'OCNTOOLSCE '`（名 8 字节 + 扩展 3 字节），写 `b'OCNTOOLS.CE '` 会匹配失败
- [ot][经验] Catos_Better_better 新内核 CE API 表：`ls` 带回调（ce_ls_cb：is_dir/name/size/ud）、FS_DATA_MAX=512 未变、cat≤8KB 未变、无 delete/append/seek/swap；内核态有 uid 权限系统（perm 表，SYS_SAVE 强制检查）；用户态"重复实现"排查应以此 API 面为基线
- [ot][易错点] sed 替换串含 `&` 时 `&` 是"整个匹配"特殊符，替换文本里的 & 必须写 `\&`，否则 C 字符串被破坏（"save&quit" 两次被 sed 改写坏，最终用 Edit 工具精确替换修复）；同样地，build.sh 内联 python 正则若匹配已删除的源码数组（BANNER[] 外置后）会报 `AttributeError: 'NoneType' object has no attribute 'group'`，改版删除源码数据时要同步改检查脚本（build.sh banner width check 已改读 scripts/BANNER.TXT）
- [ot][易错点] push_files 内联参数必须填真实文件内容，不能填占位符后"再覆盖"——占位符会真的推上去（本次 audit/aesthetic/README/scripts 全被 PLACEHOLDER 覆盖过一轮，需二次推送真实内容修正，浪费一次 commit）
- [ot内核v2.5][易错点] QEMU 环境每轮重置：qemu-system-i386/qemu-img/磁盘都会消失，开工先 `sudo apt-get update && apt-get install -y qemu-system-x86 qemu-utils`，重建 4G 稀疏盘 + QEMU 内 `format` 命令**需二次确认 y** 才真正格式化
- [ot内核v2.5][易错点] 注入脚本 8.3 名必须与内核查询名完全一致：`minimal_ce.ce` 注入为 `MINIMAL_CE.CE` 时名字段截断为 `MINIMAL_`（10 字符超 8），内核查 `/bin/minimal.ce` 生成 `MINIMAL CE ` 不匹配 → err -2；注入用与查询一致的 `MINIMAL.CE` 即可
- [ot内核v2.5][易错点] shell 管道右侧**不能带 run 前缀**：`run /bin/gen_ce.ce | run /bin/sink_ce.ce` 会把右侧整串当命令路径（file2="run"）→ err -2；正确 `run /bin/gen_ce.ce | /bin/sink_ce.ce`
- [ot内核v2.5][易错点] cpu_switch_park_and_resume 保存 prev 为**协程上下文**（push ebp/ebx/esi/edi+esp），恢复 prev 必须走 cpu_switch_to；prev->resume_frame 不清 0 会误按 ISR 帧格式弹栈（pop gs 弹垃圾段 GP）——**调度让出方必须清 resume_frame，trap 入口清 t->resume_frame**
- [ot内核v2.5][易错点] tasks[] 是静态数组，task_spawn_ce 复用槽位时新字段（out_pipe/in_pipe/out_file/out_append）残留旧值 → pipe_task_cleanup 误释放管道；**任务创建必须显式初始化所有重定向字段为 -1/0**
- [node解释器][易错点] **tok_kw 用 token 长度当比较长度**：1 字符标识符 'f'/'n' 与 "false"/"null" 首字符相同即误判关键字命中 → 函数名 f 被解析成布尔字面量（"not a function"）、参数 n 被解析成 null（"bad binary op"）；关键字匹配必须：词元长度==关键字长度 && 全词 str_eqn
- [node解释器][易错点] **n_new 只初始化 op 字段**：链尾 next / 未用 a/b/c/body/flag/num/sp 全是 arena 垃圾 → 语句链/声明符链/实参链遍历读幽灵节点（报 callee-op=3 类"not a function"、非确定 OOM）；Node 分配必须全字段清零
- [node解释器][易错点] **解析错误恢复死循环**：perr 后 p_stmt 不消费 token，p_stmt_list 无限循环分配 AST → arena 耗尽"out of memory (lex)"（且非确定）；错误恢复必须"无进展强制 tadv 推进"
- [node解释器][易错点] **单字符 '<' '>' 词法器存原始字符(60/62)，求值器期望 OP_LT=3/OP_GT=6** → "bad binary op"；parser 层要把单字符比较符映射到 OP_* 常量（'=='/'<=' 等双字符已有映射，单字符漏了）
- [node解释器][易错点] `typeof x`（无括号）解析成相邻两标识符 → perr "expected )"；实现 `typeof` 为 unary 关键字（转 E_CALL 内建）即可支持两种写法
- [node解释器][易错点] 宿主侧验证命令：`gcc -O2 -w -I extapp -o /tmp/node_test extapp/node.c extapp/host_stub.c`；CE 构建 `sh extapp/build_ce.sh node.c`（从内核根目录、文件名不带路径，带路径会 cc1 找不到文件）；改 node.c 后两步都要跑
- [node解释器][易错点] 数组越界访问返回 VAL_UNDEF → `total + arr[i]` 报 "+ needs numbers/strings"（demo 曾用 `i<5` 配 3 元素数组自踩）；脚本边界与解释器严格报错行为要区分
- [CatOS-cc编译器][易错点] **CE01 镜像必须满足内核加载器契约(task.c:1343-1345)**：entry 必须 ==0（ce_main 不在 body 偏移0 时，在 code 偏移0 插 `E9 rel32` 跳板、header entry 恒0）；api_off+4 <= image_size（api 槽放 body 末尾）；绝对地址以 body 起点=CE_LOAD_BASE 计算（不含 20B header 位移）；违反任一 → `run: cannot load (err -5)`
- [CatOS-cc编译器][易错点] cc 子集测试源不能含 `#include`（词法器对 `#` 报 bad character）；`api->` 调用为内建，无需声明
- [CatOS-QEMU][易错点] 云端每轮会话 QEMU/盘/iso 均重置：实测前必须重 make_disk + format（二次确认 y）+ 重建 b31 iso（旧 iso 无 #42 file_read/write，cc 无法运行）；pkill 进程名是 comm 截断后的 `qemu-system-i38`（15 字符限制）
