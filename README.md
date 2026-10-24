# Ocean's Tools (ot / oceanshell / osh)

CatOS i386 用户态全屏命令行（CE 应用），运行于 QEMU/Bochs 虚拟机的 CatOS 系统。

## 特性

- 全屏字符终端 shell，像素横幅 **OCEAN'S TOOLS**（大写、浅蓝 87,87,255）
- 配置文件 `/BIN/OCNTOOLS.CFG`：标题、标语、欢迎语、启动信息、启动命令
- 历史 16 条、Tab 补全、alias、多色 prompt
- **CSC 脚本语言（CSC 2.1）**：变量 32、数组 4×64、文件缓冲 fa/fb 各 8K（≤128 行索引）、for 栈 4 层、位置参数 $0-$9、字符串函数（len/upper/lower/substr/find/tok/same）、文件命令（load/loadb/rows/row/write/saveb/mergeb/dirb/clrb）、系统原语（key/peek/poke/alloc/free/arg/exists/fsize/now/pad2/revd/filen/snapb/cmpb）、整数算术
- **外置命令机制**：`/bin/*.csc` 自动注册为命令（v1.6 起 diff/vk 外置；v1.7 起 sft/vcs 外置，CSC 解释器是 ot 唯一必备内置）
- 内置工具：edt(hexedit)、fm(fmgr)、cedbg + 原 cat/ls/rm/touch/mkdir/cd/pwd 等（sft/vcs 已移至外置脚本）

## 版本

- v1.8：外置化评估 edt/fm/cedbg 保持内置（镜像余 ~8.1K）+ 收尾修复（fmgr p[-1] 边界警告、版本号 v1.7→v1.8）+ mkce.py bss 列解析 bug 修复
- v1.7：CSC 2.1 系统原语（key/peek/poke/alloc 等 19 项）+ sft/vcs 外置化（镜像体积较 v1.6 再减 2.8KB）
- v1.6：CSC 2.0 语法核心 + 外置 diff/vk（diff/vk 由 CSC 脚本实现）
- v1.4：集成 catos-ce-tools 七工具，双审闭环（功能 PASS + 审美认可）
- v1.1~v1.5：配置系统、echo 16 色、历史/Tab/alias、改名升级

## 构建

```sh
bash build.sh          # -Os 编译 → ocntools.ce（≤64KiB，含 CE01 头）
```

产物 ocntools.ce 注入 64MiB FAT32 镜像 `/BIN/OCNTOOLS.CE`。注入用 `python3 fatfs.py add_file_safe /BIN/OCNTOOLS.CE < ocntools.ce`。

## 目录

- `ocean_tools.c`  主源码（shell + CSC 解释器 + 内置工具）
- `hexedit_port.c`  edt（hexedit）移植源
- `fmgr_port.c`     fm（fmgr）移植源
- `scripts/`        外置 CSC 脚本源码（sft.csc 108 行 / vcs.csc 124 行）
- `build.sh`        构建脚本
- `OCNTOOLS.CFG`    默认配置
- 镜像 `/BIN`：`diff.csc` `vk.csc` `sft.csc` `vcs.csc` 外置脚本 + `ocntools.v15.ce`/`ocntools.v16.ce` 历史备份（可回滚）