# CatOS 文本节奏游戏 ADoFaI-mini 设计文档

> 项目目录：catos-qemu/adofai/
> 日期：2026-10-24
> 状态：架构已由 VM 实测验证（探针 probe_time / probe_io）

---

## 1. 平台约束与实测结论（2026-10-24 探针实测）

| 项 | 结论 | 来源 |
|---|---|---|
| 屏幕 | 80×25 字符，16 色，无 gotoxy API（全屏重绘走 write 整串 + \n） | API-审计 / 内核文档 |
| 颜色 | set_fg 0..15；准度条用 绿=2 / 黄=14 / 红=4（用户指定） | 用户需求 |
| 墙钟 | **delay(ms) 基于 PIT get_ticks，墙钟准确**（milli_delay: (ticks-t)*1000/HZ < ms）→ 游戏主时钟 = delay(1) 计数，1ms 档 | probe_time |
| RDTSC | 忙等 vs 内核等待态速率差 36~155 倍，**不可跨态计时** → 弃用 | probe_time |
| getch | CE API 槽22 为阻塞读，无超时无非阻塞变体 → 不能轮询 | probe_time |
| 输入 | **CE 运行于 ring0，可直读 I/O 端口**：屏蔽 IRQ1（PIC IMR 0x21 bit1）后轮询 PS/2（0x64 bit0=数据就绪，0x60=scancode）；任何 make 码 = 一次按键，帧级（~1ms）精确；退出恢复 IRQ1 并排空缓冲，shell 输入验证正常 | probe_io |
| 内核内存 | 读取内核镜像区会破坏文本控制台状态（原因未明）→ 游戏不读内核内存 | probe_kb |
| 镜像 | CE01 ≤65536B，基址 0x02000000，ce_main 直接 return 0，禁 api->exit() | build.sh/mkce.py/清单#1 |
| 栈 | 内核栈 8KiB → 所有大缓冲文件级 static | 清单#2 |
| 文件 | cat ≤8KB 读、save ≤512B/次、路径段 ≤31 字符 | 清单#7/#8/#9 |
| 浮点 | 无 → 全部整数运算 | 清单#6 |

---

## 2. 游戏架构（最终）

### 2.1 主循环（单线程、忙等帧循环）

```
init: 解析关卡 -> 预计算每 tile 的命中时刻（ms, 整数）
      屏蔽 IRQ1
loop:
  delay(1);  now++                     # PIT 墙钟，1ms/帧
  poll PS/2 端口                        # 非阻塞，捕获按键帧
  if 按键到达: 记录 press_time = now; 消费(排空多余 make)
  更新判定状态机（对应当前 tile 的命中窗口）
  if now - last_draw >= 33ms: 整帧重绘（static buffer + api->write）
  检查: 过晚未按(miss) -> 失败; 全部 tile 通过 -> 胜利
exit: 恢复 IRQ1, 排空, return 0
```

### 2.2 输入模型（PS/2 端口轮询）

- 屏蔽 IRQ1 后内核键盘 IRQ 不再取走 scancode，游戏独占 0x60。
- 按键判定：`(inb(0x64) & 1)` 为真 → `inb(0x60)` 读 scancode；
  - 0xE0 前缀：消耗后续一字节，视为一次扩展键按下（不区分具体键）；
  - make 码（bit7=0）→ 记一次按键；break 码（bit7=1）→ 忽略（松键不算）。
- 每 tile 消费一次按键；同 beat 内多余 make 立即排空（防连打干扰节奏）。
- 退出前：恢复 IRQ1（`outb(0x21, inb(0x21) & ~0x02)`）+ 排空 0x60 缓冲（≤8 次读）。

### 2.3 节拍与判定（全部整数运算）

- 基准：`ms_per_beat(bpm) = 60000 / bpm`（整数）。
- 累计避免漂移：第 i 拍（自变速点起）时刻 `t = q + (r * 60000) / bpm`，其中 `q = i / bpm * 60000` 用余数累加实现（32 位安全）。
- 变速：SetSpeed 事件按"自该 tile 起采用新 bpm/倍率"分段时间表；倍率用整数百分数（150 = 1.5x）。
- 999 midspin：该 tile 不计步、无需按键，自动通过（行星原地旋转动画）。
- Twirl：仅反转行星运动方向（视觉效果：准度条扫动方向反转），不影响计时。
- 判定窗口（**官方角度制**，已查证，2026-10-24）：
  - 行星每拍转过 180°；角度窗口 A° 对应毫秒 = A×60000/(180×BPM)（整数运算，随 BPM 收紧）；
  - Perfect!   |Δ| ≤ 30°（= 10000/BPM ms）
  - EPerfect!  −45° ≤ Δ < −30°（早侧完美，连写）；LPerfect! +30° < Δ ≤ +45°（晚侧完美）
  - Early!     −60° ≤ Δ < −45°；Late! +45° < Δ ≤ +60°
  - Early!!    Δ < −60°；Late!! Δ > +60° → miss → 失败
  - Miss...    命中窗口内无按键 → 失败
  - Δ = press_time − hit_time
  - 高 BPM 封顶（PC 官方）：Perfect ≥400BPM 固定 ±25ms（P_ms = max(25, 10000/bpm)）；E/LPerfect ≥500 固定 ±30ms；Early/Late ≥308 固定 ±65ms（max(65, 20000/bpm)）。
- 判定文本（官方游戏内浮动写法，连写/标点不可错）：`Perfect!` `EPerfect!` `LPerfect!` `Early!` `Late!` `Early!!` `Late!!` `Miss...` `Multipress!!`
- 结束文本：胜利 = `Congratulations!`（普通通关）；全部判定均为 Perfect!（且无多余按键/多按）→ `Pure Perfect!`；失败屏 = `Game over!` + 完成百分比 + `Restart!` 提示。

### 2.6 .adofai 谱面模型（已查证，2026-10-24）

- 顶层键：`angleData`(int[]，角度数组=每 tile)、`settings`(含 `beatsPerMinute` 初始 BPM、`offset`)、`actions`(事件数组，**不叫 events**)、`decorations`(忽略)。旧谱无顶层 bpm 字段。
- `angleData[i]`：0..345（15° 倍数），999 = midspin；长度=tile 数；首块通常 0。
- 事件对象：`{ "floor": i, "eventType": "..." }`；
  - SetSpeed：`speedType`("Bpm"/"Multiplier")、`beatsPerMinute`、`bpmMultiplier`、`angleOffset`(先按 0)；**无 duration 字段**（瞬时事件）；
  - Twirl：**无 direction 字段**，纯翻转开关；FreeRoamTwirl 才有 direction（忽略）。
  - Pause（可选，迷你子集可跳过）。
- 999 midspin：不产生独立判定（按键自动推进过 midspin）；**但占用一拍时间**（行星原地旋转一拍），命中该拍的按键同时推进到后续 tile。实现：beat 表包含 999（占一拍、无判定、无需按键）。
- 非标准 JSON：文件带 UTF-8 BOM、允许尾逗号、字符串内裸换行 → 迷你解析器用**字节扫描状态机**边扫边提取四类数据，不做完整 JSON 解析。
- 判定换算：每拍=60000/BPM ms；角度窗口→ms 见 §2.3（与查证公式一致：ms=(θ/180)×60000/BPM）。
- pathData（新谱字符压缩轨道）可选；迷你子集仅支持 angleData，遇到只有 pathData 的谱报 "UNSUPPORTED: pathData"。

### 2.4 画面布局（80×25，全屏 static buffer 重绘）

```
行 0:  <歌曲名 Artist - Song>             <BPM xxx>
行 1:  PROGRESS NN%    SCORE NNN    ACC NNN%    (COMBO n)
行 2:  (空)
行 7:   (准度条背景区，非中心部分)
行 8:   <--[red][yellow][green][ GREEN ][green][yellow][red]-->   准度条
行 9:   'O' 扫动标记（行星）行进方向：默认左→右；Twirl 后右→左
行 10:  判定提示词（Perfect! / Early! / ...）居中，白/蓝/红配色区分
行 12:  提示行：ANY KEY = TAP  |  ESC = QUIT
```
- 行 0 歌曲名用 `Artist - Song` 式（查证：ADoFaI 原生顶部即此格式）；进度/得分/准确率为用户需求的自定义 HUD（ADoFaI 原生无，MOD 才有）。
- 准度条：中心绿色区（Perfect 窗口对应宽度）、两侧黄色区、再外红色区；扫动标记 'O' 用 delay 墙钟位置插值，命中瞬间为行 9 居中处。
- 失败屏 / 胜利屏：全屏替换，见 §2.5。

### 2.5 结果屏

- 失败：`Game over!` + `PROGRESS: NN%`（已完成 tile / 总 tile）+ `Restart!` 提示。
- 胜利：`Congratulations!`（社区查证后定稿）；若全程 Perfect → `Pure Perfect!`。

---

## 3. .tadofai 纯文本格式（行式、易解析、整数）

```
# 注释（# 开头）
title: <歌曲名>            # 可选，HUD 显示
bpm: 120                  # 初始 BPM（整数）
# --- tiles（一行一 tile，按出现顺序） ---
tile 0                    # 普通 tile，角度为 15 的倍数（0..345）
tile 90 twirl             # 该 tile 命中后反转方向
tile 180 speed 150        # 该 tile 命中后速度倍率改为 1.5x
tile 270 bpm 100          # 该 tile 命中后 BPM 改为 100
tile 999                  # midspin：原地旋转，无判定、不计步
```
- 每行关键字：`tile`（必带角度），后缀事件：`twirl` / `speed <n>` / `bpm <n>`；
- 文件 ≤8KB（cat 上限）；tile 数上限由内存预算定（静态数组，目标 ≥256 tile）；
- 容错：忽略未知行、空行、注释；非法角度报错并拒绝加载。

## 4. .adofai 支持策略

- 目标：CE 内迷你 JSON 子集解析器（体积预算 ≤3KB），支持社区 .adofai 常用子集。
- 解析器大小约束：若编译后镜像超 64KiB，回退为**宿主转换器**（python `adofai2tadofai.py`，把 .adofai 转成 .tadofai 再导入），并如实说明。
- 游戏内选关：启动菜单列出 /BIN/ 与 /LEVELS/ 下的 .TADOFAI / .ADofAI 文件（cat 读入解析）；内置 2 个 demo 关卡以 .tadofai 形式随 CE 一起注入。

## 5. 内置 demo 关卡（2 个）

1. `DEMO1.TADOFAI` — 教学关：bpm 100，角度 0/90/180/270 循环 + 1 次 speed 200 + 1 次 twirl。
2. `DEMO2.TADOFAI` — 进阶关：bpm 140，多次 twirl 交替 + speed 150/75 变速 + 2 个 999 midspin。

## 6. 体积与内存预算

- 镜像 ≤64KiB：源码目标 ≤40KB 编译后（留余量给解析器与关卡文本）。
- 静态数据：屏幕缓冲 80×25+1 ≈ 2KB；关卡 tile 表每 tile 4B × 256 = 1KB；判定/状态变量 <256B。总计 <4KB 静态段。
- 无动态分配（不用 alloc/free），避免堆碎片。

## 7. 验证与交付流程

1. 编译 → CE01 ≤64KiB → 停 VM 注入 /BIN/ → 重启 VM；
2. QEMU 实测：菜单、游玩（准度条/HUD/判定词）、胜利、失败各截图；
3. 双审闭环：功能/正确性审查 → 审美审查（两轮换新代理）→ 迭代；
4. git commit（仅 adofai/ 目录）+ GitHub 推送；
5. 交付：CE 路径 + 截图 + 玩法/格式/操作/限制说明。

## 8. 已知限制（如实说明）

- 无音效（无声卡）；无轨道/行星全图，仅准度条扫动近似（用户明确只要这些）。
- IRQ1 屏蔽期间系统其它键盘消费者无输入（游戏独占；退出即恢复，已验证）。
- 无鼠标/触屏；仅键盘任意键。
- .adofai 仅子集；复杂谱面（Pause/MoveTrack/滤镜/多行星）不支持，报"UNSUPPORTED"跳过。
