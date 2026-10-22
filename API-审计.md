# CatOS CE API 审计（v2.2，33 调用）

来源：kernel.bin 内嵌文档（API.TXT / CEFORMAT.TXT 字符串提取）+ 虚拟机实测。

## 1. API 全景与实测状态

| 分组 | API | 实测/证据 |
|---|---|---|
| console | write / putc / cls | shell 全在用，正常 |
| input | readline / getch | shell 行输入正常；getch 供 CE 应用（demo 用） |
| time | time_str | `date` 正常 |
| fs | fs_ready / ls / touch / mkdir / cat / save | dir/mkdir/touch 实测 OK；cat/save 有实现字符串，save 受 512B 限制 |
| mem | alloc / free | demo 字符串含 alloc 测试（alloc 4096 ok / failed 分支） |
| misc | exit | run 文档明确支持 |
| net | net_init / net_up / http_get / net_dns / net_udp_send / net_udp_recv / net_ping / net_ip | ping/curl/dns 实测 OK；udp_send OK；**udp_recv 实测不通** |
| helpers | print_int / print_hex / set_fg / delay / atoi / strlen / strcmp / strcpy / file_exists / fsize | 均有实现证据；无 printf/sprintf/浮点 |

33 个 API 全部有实现痕迹，`netdiag` 内置诊断命令也已实现（PCnet 寄存器级转储实测正常）。

## 2. 已发现的问题（优先级从高到低）

1. **内置 CE 应用安装失败（阻断 CE 生态）**
   - 内核内嵌 6 个应用字节码（hello/calc/sysinfo/demo/curl/netinfo），本应安装为
     `/bin/hello.ce` … `/bin/netinfo.ce`（CE01 头 + i386 镜像）。
   - 实际装进 `/bin` 的是 `hello.cc / calc.cc / sysinfo.cc / demo.cc / curl.cc / netinfo.cc`
     （约 8KB，非 CE01 格式）→ `run /bin/hello.cc` 报 `missing / bad CE01 / too big`。
   - 整个磁盘无任何可加载的 `.ce`，全部内置程序开箱不可用。
   - 修复方向：安装环节写入的 blob 内容/文件名打包错误，需查内核 install 逻辑或 ISO 构建脚本。

2. **net_udp_recv 收不到回包**
   - `udp 10.0.2.2 9090 hello`：发包成功（宿主 UDP 服务确认收到并回 `echo:hello`），
     guest 显示 `udp: no reply`。
   - 影响：UDP 相关应用（游戏联机、简单 RPC）全部不可用。需查收包路径/socket 绑定/超时逻辑。

3. **save() 文件写上限 512B**（编辑器：SAVE FAILED: exceeds 512-byte file limit；
   行数 ≤64、行宽 ≤79）
   - 编辑 >512B 的文本文件会保存失败；CE 应用无法写大文件。
   - 影响 python.ce 写脚本/存档等场景。

## 3. 尚未实现、但按路线大概率需要的 API（内核字符串 0 痕迹）

| 能力 | 痕迹 | 需要的场景 |
|---|---|---|
| printf / sprintf 格式化输出 | 0 | python.ce 打印混合文本/数字；游戏 HUD |
| 浮点支持（float→str、运算） | 0 | python.ce 的 float 类型 |
| 随机数 rand | 0 | 游戏、脚本 |
| 文件 append / delete / rename | 0 | python.ce 写文件、游戏存档、文件管理 |
| 堆扩容（现 64 KiB 静态池） | 限 64K | python.ce 解释器（符号表+值栈）紧张 |
| 通用 TCP（socket/connect/send/recv） | 仅一次性 http_get | 工具链网络、交互协议 |
| 进程/任务、管道、环境变量 | 0 | "Unix-like" 目前是单任务 shell |
| 内存/资源统计 | 仅 mem 命令 | 调试 |

已满足、无需补：getch（含方向键 KB_*）、delay、set_fg/color、time_str、
ls/touch/mkdir/cat、net_dns/net_ping/net_ip —— 终端游戏类需求基本够用。

## 4. 建议

- 第 1、2 项是"文档承诺功能实际不可用"，优先修（用户偏好：确认后直接重编译注入）。
- 修复需要 CatOS 源码树（内核 install/udp 收包逻辑 + ISO 构建脚本 build_ce.sh/mkce.py 侧）。
  当前只有 ISO 与内核二进制，可协助定位，但二进制级 patch 风险高，不推荐。
- python.ce 落地前，建议至少补：sprintf、float→str、rand、文件 append/delete、堆扩容。
