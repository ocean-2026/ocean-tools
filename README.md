# Ocean's Tools (ot / oceanshell / osh)

CatOS i386 用户态全屏命令行（CE 应用），运行于 QEMU/Bochs 虚拟机的 CatOS 系统。

## 特性

- 全屏字符终端 shell，像素横幅 **OCEAN'S TOOLS**（大写、浅蓝 87,87,255）
- 配置文件 `/BIN/OCNTOOLS.CFG`：标题、标语、欢迎语、启动信息、启动命令
- 历史 16 条、Tab 补全、alias、多色 prompt
- **CSC 脚本语言（CSC 2.1）**：变量 32、数组 4×64、文件缓冲 fa/fb 各 8K（≤128 行索引）、for 栈 4 层、位置参数 $0-$9、字符串函数、文件命令、系统原语、整数算术
- **外置命令机制**：`/bin/*.csc` 自动注册为命令
- 内置工具：edt(hexedit)、fm(fmgr)、cedbg + 原 cat/ls/rm/touch/mkdir/cd/pwd 等（sft/vcs 已移至外置脚本）

## 版本

- v1.9：用户态 execve + 软件交换层 swap（swapout/swapin/swapinfo，512B 页映射分片，打破 512B save / 8KB cat 限制）+ 合作式权限系统（ACL.TXT：adminonly/protect，su/whoami/acl）+ 剪贴板（clp + readline Ctrl+A/C/X/V + Ctrl+P 快照 snap）+ Modal 弹窗（+-| 边框居中 [ ] 按钮，ESC 取消）+ 用户态分配器（mem）+ 网络诊断（netdiag）+ 命名空间前缀路由（net.ping/fs.cat）+ CSC L1 表达式求值器（eval）；镜像 65528B/65536B（双审收口：功能 2 轮 PASS + 审美有条件认可）；L3 与 shell 现代化转设计文档（v1.9-deferred.md），⑪ 功能包外置 calc/wc/time .csc；#34 QEMU 新实例键盘失效
- v1.8：外置化评估（edt/fm/cedbg 保持内置）+ mkce.py bss 列解析 bug 修复
- v1.7：CSC 2.1 系统原语（key/peek/poke/alloc 等 19 项）+ sft/vcs 外置化
- v1.6：CSC 2.0 语法核心 + 外置 diff/vk
- v1.4：集成 catos-ce-tools 七工具，双审闭环
- v1.1~v1.5：配置系统、echo 16 色、历史/Tab/alias、改名升级

## 构建

```sh
bash build.sh          # -Os 编译 → ocntools.ce（≤64KiB，含 CE01 头）
```

产物注入 64MiB FAT32 镜像 `/BIN/OCNTOOLS.CE`（`python3 fatfs.py add_file_safe`）。

## 目录

- `ocean_tools.c`  主源码（shell + CSC 解释器 + 内置工具）
- `hexedit_port.c`  edt（hexedit）移植源
- `fmgr_port.c`     fm（fmgr）移植源
- `scripts/`        外置 CSC 脚本源码（sft/vcs/calc/wc/time/verify）
- `build.sh`        构建脚本
- `OCNTOOLS.CFG`    默认配置
- `v1.9-deferred.md` L3 / shell 现代化设计稿 + ⑪ 状态表 + 审美遗留
