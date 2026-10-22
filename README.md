# CatOS 在 QEMU 下的运行验证记录

- ISO: catos.iso（15.5 MB，GRUB 2.06 + multiboot /kernel.bin，El Torito no-emulation，i386）
- 内核: kernel.bin 4.79 MB（ISO 内日期 2026-10-14）
- 环境: QEMU 6.2.0 (Ubuntu 22.04)，512 MB RAM，无头模式（VGA screendump + monitor 键盘注入）

## 已验证功能

| 项目 | 结果 | 说明 |
|---|---|---|
| 引导 | ✔ | GRUB 菜单 → CatOS i386 0.2 启动横幅 |
| 显示 | ✔ | VGA 80x25 文本模式，ASCII 艺术标题 |
| 输入 | ✔ | PS/2 键盘（注：本容器用 monitor 注入需慢速，否则丢键） |
| 内存 | ✔ | `mem`：Lower 639KB / Upper 523136KB / 总约 511MB |
| RTC | ✔ | `date` 正常读实时时钟 |
| IDE+FAT32 | ✔ | 64MiB 盘识别；`format` 建卷并安装目录树与内置应用 |
| 文件系统 | ✔ | `dir`/`mkdir`/`touch`/`cd` 正常（RAM fs + IDE FAT32） |
| 编辑器 | ✔ | `edit` nano 风格可用（^X 退出会提示 saved；文件未被破坏） |
| 网卡 | ✔ | 需 `-device pcnet`；默认 e1000 不识别。启动后 `AMD PCnet-PCI up (10.0.2.15)` |
| ping | ✔ | `ping 10.0.2.2` 3 次 reply |
| curl | ✔ | `curl http://10.0.2.2:8088/test.txt` 取回 36 字节 |
| dns | ✔ | `dns example.com` → 172.66.147.243 |
| udp | ⚠ | 发包成功（宿主机收到 hello），等待回包超时显示 `udp: no reply` |

## 待开发者确认的问题

1. **内置 CE 应用无法运行**：`run /bin/hello.cc` → `cannot load (missing / bad CE01 / too big)`。
   `/bin` 内安装的是 `hello.cc / calc.cc / sysinfo.cc / demo.cc / curl.cc / netinfo.cc`（源码文本，
   约 8KB，无 CE01 magic），而 `run` 需要 `.ce` 可执行镜像（CE01 头 + i386 裸镜像），
   且整个磁盘上没有任何 `.ce` 文件 → 开箱即无可运行的应用。
   已确认内核内嵌的 6 个应用字节码本身在 kernel.bin 里（install 环节写错了文件名/内容）。
   注：CEFORMAT.TXT 与 `run` 报错均写 magic 为 `"CE01"`（早期截图 OCR 误读为 CD01，已复核）。

2. **UDP 收包路径**：宿主机 UDP 服务确实收到 `hello` 并回 `echo:hello`，但系统显示 `udp: no reply`。
   建议检查回包接收/等待逻辑或超时窗口。

3. **编辑器退出行为**：`^X` 输出 `edit: saved 512 bytes.`（文件实际 2093 字节，未被截断），
   提示信息疑似指编辑缓冲而非磁盘文件，容易误导。

## 运行方式（本目录）

```bash
bash run.sh                 # 启动（已含 pcnet 网卡 + 64MiB IDE 盘 + 无头显示）
python3 shot.py screen-NN   # VGA 抓屏
python3 typecmd.py "命令"   # 键盘输入（0.3~0.5s/键，丢键请重试）
python3 qmon.py /tmp/catos-mon.sock "quit"   # 关闭
```

宿主机测试服务（供 curl/udp 复测）：
- HTTP :8088（目录 /tmp/webtest，文件 test.txt）
- UDP 回显 :9090（脚本 /tmp/udpecho.py，日志 /tmp/udp.log）

注：本容器里 `qemu-img`/`-drive` 对普通文件的写锁会失败（环境文件系统问题），
所以 run.sh 用 `dd` 建盘 + `-blockdev locking=off` 规避；在正常 Linux 主机上直接 `-drive file=disk.img,if=ide` 即可。

## 截图索引（screen-*.png）

- screen-01 GRUB 菜单；screen-04 启动横幅（网卡 up 10.0.2.15）
- screen-12 uname/date/mem；screen-15 format 完成、目录树安装
- screen-16 dir /bin（内置 .cc 应用）；screen-21 ping；screen-22 curl；screen-24 dns
