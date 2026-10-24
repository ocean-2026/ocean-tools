# experience.md - 开发经验记录

格式：[所属项目][经验]内容

- [ot][经验] 黑盒探测优先于猜测：execve 实现前用 argv_probe.c 实测内核 CE 调用约定（argv@0x0147c500/字符串@0x0147c540/esp/ce_main@0x02000000/api 表@0x0101c280），所有常量来自实测而非文档推导
- [ot][经验] 裸函数（naked）内无操作数 asm 用**单 %** 寄存器，扩展 asm（有操作数）用 **%%**；混用报 "bad register name"（exec_tramp 踩过）
- [ot][经验] QEMU sendkey 数字参数（0xNN）语义在 QKeyCode/PS/2 扫描码间不可靠（实测 0x2d 时 x 时 v），**键名模式（sendkey c）更稳定**；长 hold 参数（sendkey o 1000）不改善 CatOS 接收率
- [ot][经验] 跳板/桩代码用 `__attribute__((naked))` + 纯汇编 + `exec_tramp_end` 取长度差拷贝，避免内联函数地址问题；用 objdump 反汇编核对每条指令机器码再进 VM
- [ot][经验] exec 场景（覆写自身镜像）的调试节奏：先在宿主侧完成全部静态验证（CE01 头解析、跳板寄存器契约、反汇编），VM 只做最终确认，减少对脆弱键盘通道的依赖
- [ot][经验] 内核栈 8KiB 限制下，跳板/argv 区选在探测到的固定安全区（0x0147c500-0x0147c800），避开镜像（0x02000000）与内核栈（esp≈0x0147e658），先算布局再写代码
