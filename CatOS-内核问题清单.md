# CatOS 内核问题清单（CE 用户态开发视角）

> 汇总 ot（ocntools.ce / ocean_tools）系列开发以来遇到的内核侧限制、bug 与怪癖。
> **维护约定：每次开发遇到新的内核问题，就在对应分类下追加条目**（含确认值、影响、规避方式、发现版本）。

---

## 一、内存与执行模型

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 1 | CE 镜像大小上限 | ≤65536 字节（CE01 头，build.sh/mkce.py 检查） | ocntools.ce v1.6=60120B、v1.7=57400B，贴近上限 | 持续瘦身；体积敏感的代码尽量外置 .csc | v1.0 起 | 已确认 |
| 2 | 内核栈仅 8KiB | 8 KiB | CE 程序大栈帧会踩栈（v1.6 B1：run_script_args 栈上 buf[8192]+lines[128][97]≈21.4KB） | 大缓冲必须文件级 static；v1.6 已把 CSC 解释器缓冲 static 化 + g_csc_active 防重入 | v1.6 | 已确认/已修复 |
| 3 | 堆为 64KiB 静态池 | api->alloc/free，64 KiB 静态池 | alloc 失败即池耗尽；大块分配需及时 free | alloc 后立即 free；脚本里 alloc/free 配对 | v1.7 | 已确认 |
| 4 | 无固定地址 alloc + 固定链接基址 0x02000000 | 链接基址固定，无固定地址分配 | CE 程序无法动态加载/运行其他 CE（ot 不能 run .ce） | cecheck 作为用户态安全等价物（不依赖内核加载器） | v1.2 评估 | 已确认/已规避 |
| 5 | api->exit() 有 bug：重载同一镜像 | exit() 会 relaunch 同镜像 | 调用 exit() 导致程序反复重启 | ce_main 直接 return 0，禁止调 api->exit() | v1.0 起 | 已确认/已规避 |
| 6 | 无浮点运算 | 内核 API 无浮点 | 数学只能整数 | 全部整数运算 | v1.0 起 | 已确认 |

## 二、文件系统（FAT32）

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 7 | save() 单次写入上限 | ≤512 字节/次 | 大文件无法一次保存（vcs/edt 需分片） | 分片保存（shards）；CSC 侧配合 save 分片 | v1.4（vcs/edt 集成） | 已确认 |
| 8 | cat/load 读取截断 | ≤8KB | 超过 8K 的文件读不全（CSC 脚本同限） | 脚本 ≤8K/≤128 行；大文件用工具分片读 | v1.0 起 | 已确认 |
| 9 | 路径段长度上限 | ≤31 字符（FAT 短名） | 长文件名不可用 | 命名控制 ≤31 | v1.0 起 | 已确认 |
| 10 | resolve_existing 解析绝对路径最长 | ~127 字符 | 深路径装入小 buffer 会越界（v1.6 M2：vcs base[96] 等 5 处） | 槽位按 ≥160 预留（已修 base[160]/path[160]） | v1.6 | 已修复 |
| 11 | 目录查找大小写不敏感 | resolve_existing 两级解析（cwd+/bin） | 路径大小写随意 | 解析层统一处理；fatfs 写入路径段大写 | v1.0 起 | 已确认 |
| 12 | disk.img 路径段规范 | 路径段大写（FAT32） | 小写段可能解析歧义 | fatfs.py 操作时路径段大写 | v1.0 起 | 约定 |

## 三、显示与输入

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 13 | 屏幕规格 | 80×25 字符、16 色、无浮点 | 全屏工具布局受 80 列限制（状态栏 >79 列会折行） | 状态栏压 ≤79 列；整屏重绘约束内 | v1.4 | 已确认 |
| 14 | 无背景色 API | set_fg 无背景色位 | fmgr 选中行无法真反白 | 亮白 + `>` 前缀近似（审美评审认可） | v1.4 | 已确认/已近似 |
| 15 | CE 键盘键码 | UP=0x100 / DOWN=0x101 / TAB=0x09 / ESC=0x1B（api->getch） | 方向键是非 ASCII 值 | 按键分支按 CE 键码判 | v1.2 起 | 已确认 |
| 16 | 等宽字体换算基准 | 1 空格 = 1 字符 = 9px；1 行 = 16px | 图形/布局换算（如标题居中、plot 坐标） | 以 9px/16px 为基准计算 | v1.2 | 已确认 |

## 四、工具链与加载

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 17 | mkce.py 的 bss_size quirk | 把 readelf 的 .bss **文件偏移**当 bss_size 写入 CE01 头（真实 .bss=126224B） | 头字段值可疑，实测运行正常 | 暂沿用；建议后续单独核对内核加载逻辑（v1.6 审查跟进项） | v1.4 起 | 待核对 |
| 18 | CE01 头 / 基址 / 重定位 | magic/entry/bss_size/api_offset/image_size 实测正确；基址 0x02000000；无重定位 | 正常 | — | v1.6 审查 | 已确认 |
| 19 | QEMU sendkey 丢键 | 空格/字母/回车偶发丢失（测试环境噪音） | 长命令粘连、验证误判 | 慢速发送 + vga 屏幕回显校验 + BS 清行重试 | v1.0 起 | 环境噪音（非内核） |
| 20 | fatfs.py 工具 bug/口径 | dump_tree 递归 TypeError 勿用；read_chain 参数是簇链 list 非 slot 号 | 误用会崩或读错 | 用 add_file/delete_file/read_chain（簇链） | v1.0 起 | 工具备忘 |
| 21 | 测试环境每轮重置 | Ubuntu 云环境 qemu 等配置可能每轮消失 | 直接测试会失败 | 开工先检查 qemu/gcc/git 配置（AGENTS.md 规则） | v1.0 起 | 环境规则 |

## 五、未决问题

| # | 问题 | 说明 | 状态 |
|---|---|---|---|
| 22 | 用户曾问 "512k 是哪个" | 已知确认值仅 **512B**（save 上限）；未在历史轨迹中确认 512K 出处（可能是显示/显存或其他资源的 512K，也可能是笔误） | 待查证 |

---

## 扩充规范

1. 遇到新的**内核侧**问题（API 限制、加载器行为、文件系统约束、显示/输入怪癖）→ 在此文档追加条目；
2. 每条必须含：问题、确认值、影响、规避/对策、发现版本、状态；
3. 状态值：已确认 / 已修复 / 已规避 / 待核对 / 环境噪音（非内核）/ 待查证；
4. 工具链问题（fatfs.py、QEMU 等）单独归入"工具链与加载"，与内核问题区分标注。

## 六、ADoFaI 游戏项目新增实测（2026-10-24）

| # | 问题 | 确认值 | 影响 | 规避/对策 | 发现版本 | 状态 |
|---|---|---|---|---|---|---|
| 23 | delay(ms) 基于 PIT get_ticks（milli_delay），墙钟准确 | milli_delay: `while(((get_ticks()-t)*1000/HZ)<ms)` | delay 可作游戏主时钟（1ms 档） | 游戏时钟=delay(1) 计数 | adofai 探针 | 已确认 |
| 24 | RDTSC 跨状态速率不稳（忙等 vs 内核等待差 36~155 倍） | 忙等≈7.2M tick/s；getch 等待期间虚拟时钟速率剧变 | 不能作为跨 getch 的墙钟 | 弃用 RDTSC；只用 delay | adofai 探针 | 已确认 |
| 25 | CE getch（槽22）阻塞；键先入内核 kb_in 队列 | 无键时挂起，实测等待 60s+ 不返回 | 无超时→无法轮询输入 | 改用 PS/2 端口直轮询（见 #26） | adofai 探针 | 已确认 |
| 26 | CE 可直接端口 I/O（ring0）；屏蔽 IRQ1 后轮询 PS/2 可行 | PIC IMR 0x21 bit1；inb 0x64/0x60；实测 5 键全捕获 | **实时非阻塞输入通道**：任何 make 码=一次按键（帧级 1ms 精确） | 游戏内掩 IRQ1→轮询→退出恢复+排空 | adofai 探针 | 已验证（新能力） |
| 27 | 文本控制台 framebuffer 不在 0xB8000 | pmemsave 0xB8000 为陈旧内容，与 screendump 不符 | 不要用 0xB8000 推断屏幕状态 | 用 screendump / 屏幕回显校验 | adofai 探针 | 已确认 |
| 28 | 读取内核内存 [0x1000000..0x14918b4] 会破坏文本控制台状态 | 全区间扫描后控制台乱码（原因未明） | 避免扫描内核内存 | 游戏输入走端口轮询，不读内核内存 | adofai 探针 | 已确认/已规避 |
| 29 | typecmd.py/type_slow.py 数字键码 off-by-one | DIGITS 映射把 '2' 发成 '3' 的键码（0x02+i 应为 0x02+i-1） | 含数字的命令会输错 | 已修复两个脚本 | adofai 项目 | 已修复（工具） |
| 30 | QEMU 运行中宿主侧 fatfs 注入会被 VM 写回缓存覆盖 | 运行时 add 的文件 VM 看不到 | 测试注入无效 | 注入前停 VM，注入后重启 VM；add_file_safe 重复注入会生成重复条目 | adofai 项目 | 工具备忘 |
| 31 | CE 菜单阶段（内核 getch）按键随机丢失：单独发 '1' 可启动游戏，伴随其它按键或快速连发时 '1' 频繁丢失；游玩阶段屏蔽 IRQ1 直轮询 PS/2 则 100% 可靠 | 疑似 shell 后台任务与 CE 竞争内核 kb_in 队列，或 IRQ1 路径快速按键溢出 | CE 应用应全程屏蔽 IRQ1 直轮询 PS/2（菜单/结果/游玩统一），readline 等内核输入路径临时恢复 IRQ1 | adofai 项目实测 | 已确认/已规避 |
| 32 | mkce.py 的 readelf .bss 列解析 bug（取 offset 当 size）→ CE01 头 bss 字段偏小，内核只清零部分 BSS，静态数组尾部为垃圾值 | readelf -S 第5列 offset vs 第6列 size | 已修复（adofai/mkce.py 取第6列）；ocean_tools 的 mkce.py 若同源需同步修复 | adofai 项目 | 已修复（工具） |
| 33 | QEMU monitor screendump 偶发不写文件（同一命令多次重试可成功），且 screendump 实际捕获状态比发出时刻滞后 ~1.5-2s | 工具备忘 | 测试脚本需重试 + 容忍滞后；判断状态用色彩统计（绿=游玩准度条/黄=胜利/红=失败屏） | adofai 项目 | 工具备忘 |
| 34 | QEMU 新实例 PS/2 键盘注入完全失效 | 新启动 QEMU 实例（12+ 实测）键盘 0% 接收率（HMP sendkey 键名/数字、QMP input-send-event、长 hold、bios.bin/bios-256k.bin、v1.8/v1.9 磁盘均无效），仅长时间运行的老实例部分正常（~80%） | VM 交互测试前先单键探测；备用：STARTUP_CMD 自动执行路径可绕过键盘（ot 启动后自动跑）；根因待查（疑似 CatOS 键盘驱动初始化时序/IRQ1 状态） | ot v1.9 | 环境问题/未决 |
| 35 | QEMU sendkey 注入存在不可控秒级延迟 + 键排队（2026-10-24 晚新证据） | sendkey 1 后 0.6s screendump 拍到的是注入前状态（菜单屏），键约 1~2s 后才被 CE 处理（此前 probe 轮"sendkey 1 → 失败屏"即为延迟送达）；菜单阶段 spc/数字键同样丢/延迟；10 连拍轮询 screendump（0.25s 间隔）全拍到同一失败屏=键未送达或排队未消费 | 节奏游戏"进关→LEADIN 1.2s→游玩"窗口截图时机失效：screendump 时游戏还没开始或已失败；精准跟拍判定词/胜利屏基本不可行 | ①screendump 轮询 + 色彩统计判断当前屏（绿=游玩准度条/黄=胜利/红=失败屏，单键即可区分）②连发键等延迟送达后消费 ③STARTUP_CMD 自动执行绕键盘 ④O 扫动准度条只能在 LEADIN 期间出现，属最稀缺画面 | 2026-10-24 adofai 实测 | 环境问题/未决 |

## 七、Catos_Better_better 内核能力现状（2026-10-27 用户上传版，只读核查）

| # | 能力/限制 | 确认值（源码证据） | 对 ot 的影响 | 处置 |
|---|---|---|---|---|
| 36 | 内核态权限系统（uid perms） | task.c:544-620 perm_tab/fs_perm_set/fs_perm_check 强制；SYS_SAVE 走 sys_save_uid（uid 检查失败返 -13）；fs_perm_seed 预设 /home /tmp 0777、/bin /appdevelop /etc 0755 | ot 用户态 ACL 与内核重复 → 已去除 ot 侧 ACL | 已去除重复；内核强制权限接管写保护 |
| 37 | **读权限内核未查**（cat 可读任何文件） | sys_save_uid 只查写；SYS_CAT 无 perm 检查 | ot `cat` 移除 ACL 保护检查后现可读任意路径 | 若需读保护属内核功能需求 → 记此清单，**不擅自修内核** |
| 38 | **CE 无 uid 查询 API** | ce.h 33 API 无 whoami/uid 槽位；uid 仅 task 内部 | ot 无法显示/对齐内核 uid | 若需 uid 查询 API → 内核功能需求，记此清单 |
| 39 | save 上限 FS_DATA_MAX=512B 未变 | include/fs.h:13 `#define FS_DATA_MAX 512`；sys_save_uid `if (size > FS_DATA_MAX) return -7` | ot swap/分片协议仍必要（非重复） | 保留 swap；若内核提升上限再评估去除 |
| 40 | ls 带回调（结构化目录列表） | include/ce.h:50 ce_ls_cb(is_dir,name,size,ud)；task.c VS_LS_THUNK | ot 已直接用回调式 ls，无重复实现 | 无需动作 |
| 41 | 无磁盘 swap/分页文件 | paging.c 为内存分页（frame allocator）；CE API 无 swap | ot 的"分页文件"（swap）非重复实现 | 保留；用户点名的"分页文件"实测为内存分页 |

## 八、GNU 工具链规划——内核侧需求（2026-10-28 用户提出，只记录不实施）

| # | 能力/限制 | 现状（源码证据） | 对工具链的影响 | 处置 |
|---|---|---|---|---|
| 42 | **大文件读写**：FS_DATA_MAX=512B、无 seek/append/truncate/大块读 | include/fs.h:13 FS_DATA_MAX=512；sys_save_uid `if (size > FS_DATA_MAX) return -7`；SYS_CAT 上限 ~8KB | 编译器/链接器产出 .o/可执行文件均 >512B，无法落盘 | 工具链自举硬门槛 → 内核需大块读写 API（read/write @ offset + append/truncate）；当前只记录 |
| 43 | **CE 镜像上限 64KB** | CE_MAX_IMAGE 64K / CE_MAX_BSS 256K（include/ce.h） | 真实 cc1/ld 镜像轻易超 64KB | 需提升上限或分段加载/动态加载器；当前只记录 |
| 44 | **无管道/进程间通信 + 退出码** | CE API 33 槽无 pipe/spawn 等待；task_spawn_ce 仅内核 shell 用；CE 无 fork | make 依赖管道（|）与子进程退出码判断成败 | 需 pipe(FIFO) 抽象 + 子进程退出码传递 + shell 重定向；当前只记录 |
| 45 | **可执行文件语义**（exec 位/读保护） | fs_perm_check 只强制写（SYS_SAVE）；无 exec 位 | 工具链需要"可执行文件"概念 | 属 #37 延伸；当前只记录 |
| 46 | **标准 syscall 入口面（libc 层）** | CE API 为 33 槽自定义（ce.h），非 POSIX | GCC 编译产物需统一 ABI（exit/read/write/open/malloc） | 用户侧 libc 封装层可先做；内核侧可选提供 POSIX 兼容入口；当前只记录 |