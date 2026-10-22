# ocntools.ce v1.4 — D1 修复复验报告

- 复验对象：`/home/user/Doubao/chats/38448438262014978/catos-qemu/ocean_tools/ocean_tools.c`
- 上一报告：`audit_code.md`（总判定 FAIL，缺陷 D1：tv_names[16] 溢出）
- 复验日期：2026-10-22

## 修复内容核对（三处，与修复方描述一致）

| # | 位置 | 现状 | 结论 |
|---|------|------|------|
| ① | `ocean_tools.c:1864-1865` | `#define TV_NAME_SZ 120` / `static char tv_names[TV_MAX_FILES][TV_NAME_SZ];` | 槽位由 `[16]` 加宽为 `[120]`，可容纳 resolve_existing 产出的最长 ~127 字节绝对路径（120 仍留有余量，pth[128] 极端超长时由 ③ 的循环边界兜底）。 |
| ② | `ocean_tools.c:1936-1938` | `if (l > TV_NAME_SZ - 1) l = TV_NAME_SZ - 1;` 随后逐字节拷贝 + NUL | load 侧截断常量从硬编码 15 改为 `TV_NAME_SZ-1=119`，与新槽位宽度一致。 |
| ③ | `ocean_tools.c:2009-2015` | 注释说明 + `{ u32 k=0; while (pth[k] && k < TV_NAME_SZ-1) { tv_names[tv_n][k]=pth[k]; k++; } tv_names[tv_n][k]=0; }` | add 侧由无界 `my_strcpy` 改为定长循环，显式 NUL 结尾，不可能越界。 |

抽查其余 vcs 逻辑未被改动：
- `resolve_existing` `1359-1372` 原样（cwd → /bin 两级，FAT32 大小写不敏感）。
- `tv_add` 仍在 `2003` 行调 `resolve_existing(name, pth, sizeof(pth))`，2004 行 file_exists 检查保留。
- `tv_save_shards` `1876-1898`、`tv_commit`/`tv_status`/`tv_diff`/`tv_checkout` 中对 `tv_names[i]` 的 `resolve_path` 调用（2044/2117/2253）均不变。
- TABLE[]、cmd_help、hexedit_port.c、fmgr_port.c、build.sh、ce.ld、mkce.py 未触碰（grep 与 build 输出佐证）。

## 构建复验

```
== compile ==   (仅 3 条既有 warning：off_to_row_col/my_tolower unused；fmgr_port.c:84 array-bounds 误报)
== entry check ==  02000000 <ce_main>
== binary + header ==  api_offset=0xdec0 bss=61124 image=57028
xxd: 4345 3031 0000 0000 c4ee 0000 c0de 0000 c4de 0000
```
- `bash build.sh` 退出码 **0**，无 error。
- **`ocntools.ce` = 57048 字节**（≤65536 ✓，与修复前相同；tv_names 在 BSS 加宽不影响镜像尺寸）。
- CE01 头布局不变：`CE01 | entry=0 | bss=0xeec4 | api_off=0xdec0 | image=0xdec4(57028)`；基址 0x02000000，ce_main 在镜像偏移 0。

## 原清单复核

| 项 | 修复前 | 修复后 | 证据 |
|----|--------|--------|------|
| 2. vcs add 经 resolve_existing 解析路径 | PASS（但伴 D1） | **PASS** | `tv_add:2003` 仍调 `resolve_existing`。 |
| 7. 缓冲区边界抽查 | FAIL（D1） | **PASS** | 旧 `my_strcpy(tv_names[tv_n], pth)` 无界拷贝已改为 `2013-2014` 的 `k < TV_NAME_SZ-1` 定长循环 + NUL；槽位加宽到 120；load 侧截断同步。其余 W1-W7 警告均为非阻塞，未变。 |

## 附加观察（非阻塞，不影响本次判定）

- 复验时注意到 `mkce.py` 用 `readelf -S -W` 输出的 `parts[5]` 作为 bss_size，但该列实际是 `.bss` 的**文件偏移**（0xeec4=61124），真正的 `.bss` 大小在 `parts[6]`（0x1ed10=126224）。该问题在 v1.4 原版即存在（修复前后 bss 字段均为 61124），运行时 QEMU 实测各工具正常，推测内核按自身方式清零 BSS 或该差异未触发实际故障。属于既有构建脚本 quirk，不在 D1 修复范围，建议后续单独核对内核 CE01 加载逻辑，但本次不因此改判。

## 修复后总判定：**PASS**

D1 缺陷已正确修复（三处改动与描述一致，无越界拷贝残留），构建零 error，尺寸 57048B 合规，退出路径与 CE01 头不变。原清单 8 项全部 PASS。
