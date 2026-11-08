# 实验三：Trap 入口与中断、异常处理

> 实验记录日期：2026-11-06；文字修订日期：2026-11-07。构建过程和评分输出沿用原实验记录，本次修订未重新运行工程。

## 材料来源与分工

| 成员 | 学号 | 报告使用的材料 |
| --- | --- | --- |
| 胡博延 | 2513387 | Lab3 工程与测试脚本、Lab2 内存管理代码、原实验构建和评分记录 |

## 1. 实验目标与整体流程

本实验在 Lab2 物理内存管理的基础上，完成 Trap 入口、上下文恢复以及中断、异常处理。核心任务是让 CPU 进入处理程序后，能够保留原执行现场，并在处理结束后从正确位置继续执行。

```text
硬件进入 Trap → 汇编保存现场 → C 函数分派与处理 → 汇编恢复现场 → sret 返回
```

构建时，Makefile 编译 `kern/`、`libs/` 下的源文件，通过链接脚本生成内核，再由 `objcopy` 生成 `bin/ucore.img`。本工程的内核链接地址为 `0xFFFFFFFFC0200000`，镜像加载地址为 `0x80200000`。

启动后，`entry.S` 建立启动页表、设置 `satp` 和内核栈，再进入 `kern_init`。初始化流程为：

```text
清零 BSS → 解析 DTB → 初始化控制台 → 打印内核符号
→ idt_init → pmm_init → idt_init → clock_init → intr_enable → 主循环
```

`idt_init` 将 `__alltraps` 设为统一 Trap 入口。Trap 发生后，`SAVE_ALL` 在当前内核栈上建立 `struct trapframe`，`trap()` 根据原因调用相应处理函数；返回时，`RESTORE_ALL` 恢复现场，`sret` 恢复执行位置和相应状态。

本工程仍使用 `default_pmm_manager`。为使 Lab3 骨架能够构建，需要补入 Lab2 的 `page_init()` 和 `best_fit_pmm.c`；具体问题见第4节。

## 2. 核心机制与实现分析

### 2.1 硬件入口与 stvec

本实验处理的是进入 S 模式的 Trap。硬件会记录返回地址 `sepc` 和原因 `scause`，更新 `sstatus`：用 `SPP` 记录原特权级，用 `SPIE` 保存原 `SIE`，并将 `SIE` 清零。同步异常的 `sepc` 通常指向发生异常的指令；中断的 `sepc` 用于返回被中断的执行流。

`stval` 的内容取决于 Trap 类型和实现，可能是相关地址、非法指令编码，也可能为0，不能统一理解为“出错地址”。本工程用 `sbadaddr` 这一旧名称访问对应 CSR。

硬件不会自动保存通用寄存器。本工程的入口设置为：

```c
write_csr(sscratch, 0);
write_csr(stvec, &__alltraps);
```

`__alltraps` 按4字节对齐，地址低两位为 `00`，因此 `stvec` 工作于 Direct 模式，进入 S 模式的中断和异常都跳转到同一入口。`stvec` 中还包含模式位，不能在所有模式下都把它的完整数值直接当作跳转地址。

### 2.2 SAVE_ALL 与 Trap frame 布局

`struct trapframe` 包含32个通用寄存器槽位，以及 `status`、`epc`、`badvaddr`、`cause` 四个字段。RV64 下每个槽位8字节，总大小为288字节。

| 偏移（字节） | 保存内容 |
| --- | --- |
| 0 | x0，即 zero |
| 8 | x1，即 ra |
| 16 | x2，即陷入前的 sp |
| 24、32 | x3（gp）、x4（tp） |
| 40～248 | x5～x31，按寄存器编号排列 |
| 256 | sstatus |
| 264 | sepc |
| 272 | stval / sbadaddr |
| 280 | scause |

`SAVE_ALL` 的关键是先保存旧值，再使用临时寄存器：

1. 将原 sp 暂存到 `sscratch`，然后令 sp 减去288，为 Trap frame 分配空间。若原栈满足16字节对齐，这一步仍保持对齐。
2. 按寄存器编号保存通用寄存器，暂时跳过 x2。此时 x2 已变为新栈帧基址，不能直接把它当作原 sp 保存。
3. 用 `csrrw s0, sscratch, x0` 取回原 sp，并清零 `sscratch`。s0 是 x8，其原值已保存在偏移64字节处，因此可以安全地作为临时寄存器。
4. 使用 s1～s4 读取四个 CSR。它们的原值已保存：s1（x9）在72字节处，s2（x18）在144字节处，s3（x19）在152字节处，s4（x20）在160字节处。
5. 将原 sp 写入偏移16字节处，再将四个 CSR 写入偏移256～280字节处。

保存完成后，当前 sp 指向完整的 Trap frame，汇编中的槽位与 C 结构体字段一一对应。

### 2.3 汇编入口如何调用 C 函数

`__alltraps` 在保存现场后执行：

```asm
mv a0, sp
jal trap
```

RISC-V 调用约定用 a0 传递第一个参数，因此 C 函数收到的是 `struct trapframe *tf`。`jal` 虽然会改写 ra，但陷入前的 ra 已经保存，之后可从 Trap frame 恢复。C 函数可以建立自己的栈帧，但返回时必须恢复调用前的 sp。

`trap()` 根据 `cause` 区分中断与同步异常，再调用 `interrupt_handler()` 或 `exception_handler()`。处理函数可以修改 `tf->epc`；恢复阶段必须使用修改后的值。`trap()` 返回后，执行流顺序进入 `__trapret`。

### 2.4 RESTORE_ALL 与 sret

恢复时先从 Trap frame 取回 `sstatus`、`sepc`，再恢复通用寄存器，最后恢复 sp：

```asm
LOAD s1, 32*REGBYTES(sp)
LOAD s2, 33*REGBYTES(sp)
csrw sstatus, s1
csrw sepc, s2

# 从各自槽位恢复 x1、x3～x31
# ……
LOAD x2, 2*REGBYTES(sp)
sret
```

这段代码只展示顺序，省略处仍须恢复全部相应寄存器。s1、s2 用作临时寄存器后，也要从偏移72、144字节处恢复原值。

**sp 必须最后恢复。** 前面的加载都依赖当前 sp 定位 Trap frame；最后一条加载先用当前 sp 计算地址，再把读取出的原 sp 写入 x2，从而释放此次栈帧。

x0 恒为0，无需恢复；以 x0 为目标的加载不能改变其值，但仍可能产生访存行为，因此不应加入无意义的加载。`scause`、`stval` 用于记录原因，不承担 Trap 返回控制，无需写回。

本实验保存的 `sstatus.SIE` 为0，恢复期间不会响应 S 模式时钟中断。随后 `sret` 令 `pc ← sepc`，按 `SPP` 恢复特权级，令 `SIE ← SPIE`，并将 `SPIE` 置1、`SPP` 清0。

### 2.5 中断处理与定时器重设

`scause` 的最高位标识中断，其余位表示原因号。提取原因号时应清除最高位；移位写法需要保证操作数为无符号类型，否则符号扩展可能影响结果。仅取低8位虽能覆盖本实验的小编号，也不适合作为通用实现。

| 分支 | 处理行为 |
| --- | --- |
| `IRQ_S_TIMER` | ticks 自增，调用 `clock_set_next_event()`，每100次打印一次统计信息 |
| `IRQ_S_SOFT` | 输出 `Supervisor software interrupt` |
| 其他中断 | 输出原因号，并调用 `print_trapframe(tf)` |

`clock_set_next_event()` 通过 SBI 设置下一次定时事件。**重新设置未来的触发时间，才能形成预期的周期性计时。** 若省略这一步，可能不再产生新的正常计时事件，也可能因待处理中断未解除而反复进入处理程序；不能简单概括为“只触发一次”。

按原实验环境记录，计时频率为10 MHz，间隔为100000个计数，因此一次时钟中断约间隔10 ms，每100次约为1秒，实际时间还受处理开销和运行环境影响。`SHUTDOWN_AFTER = 10` 表示打印10次后关机，用于给测试留出足够运行时间。

### 2.6 异常处理与返回地址推进

本实验对断点、非法指令输出提示，并跳过相应指令；其他异常输出现场。这是实验规定的处理方式，不能推广为真实操作系统对所有非法指令的通用策略。

测试中的 `c.ebreak` 为2字节，`.word 0x0000007b` 为4字节。针对本实验涉及的16位、32位编码，辅助函数为：

```c
uint16_t insn = *(uint16_t *)epc;
return ((insn & 0x3) == 0x3) ? 4 : 2;
```

若对 `c.ebreak` 固定加4，返回地址将落到后续32位指令中间；若不推进，则会再次执行断点。正确的推进长度是恢复执行流的关键。

这一判断只覆盖本实验的两类指令长度，不是完整的 RISC-V 变长指令解码器。直接读取 epc 处内存也依赖该地址可访问；更完整的异常处理还需要考虑读取指令时再次发生异常的情况。

### 2.7 测试如何覆盖执行路径

`tools/inject.py` 在工程临时副本中插入测试，原始 `kern_init` 不需要为评分额外改写。

```text
第二次 idt_init
→ 断点与非法指令测试
→ clock_init
→ 检查 ticks 初值
→ intr_enable
→ 等待 ticks 达到200并输出结果
```

前两类测试在开启时钟中断之前验证异常路径；时钟测试在开启中断之后验证重复进入与返回。具体命令和原记录输出集中列于第7节。

## 3. 各练习的 SPEC 与提示词

### 3.1 提示词设计

提示词围绕四件事组织：修改位置、可用接口、实现约束和验证条件。其中，Trap frame 布局与调用约定用于约束汇编入口；原因号、返回地址和定时器重设用于约束 C 层处理。

以下保留原报告记录的提示词，不将修订后的理解追写成当时的提示词。需要区分：其中“取低8位”和“2/4字节判断”是面向本实验测试范围的约束，适用边界见第2节。

### 3.2 任务一：Trap 入口与上下文恢复

对应文件：`kern/trap/trapentry.S`。提交的提示词如下：

在 kern/trap/trapentry.S 中补全标记为 lab3 function1 的两处代码：实现 RESTORE_ALL 宏，并在 __alltraps 的 SAVE_ALL 之后加入把当前 Trap frame 传给 C 函数 trap 的调用。该模块负责在 SAVE_ALL 已建立的 Trap frame 基础上恢复处理器上下文，并建立汇编 Trap 入口与 void trap(struct trapframe *tf) 之间的调用关系，使 trap() 返回后能够顺序落入 __trapret 完成 sret。只修改 trapentry.S 中的这两处，不修改 SAVE_ALL 的保存逻辑、__trapret 标号、sret 指令、struct trapframe 和 trap() 的函数签名，也不改动 trap.c、trap.h、测试文件、链接脚本和 Makefile；该模块不涉及 C 语言前向声明。可参考 SAVE_ALL 的寄存器保存顺序与固定偏移、libs/riscv.h 中的 REGBYTES、STORE、LOAD 宏、struct trapframe 的字段布局、RISC-V 使用 a0 传递第一个参数的约定，以及 sret 依据 sstatus 和 sepc 返回的机制。请直接修改真实文件并保存，不要只输出示例汇编；不要求采用固定的指令序列，也不对时间复杂度提出要求。

这个模块需要处理以下场景：

- Trap frame 定位：RESTORE_ALL 以进入时的 sp 作为 Trap frame 基址，严格按照 SAVE_ALL 建立的 36*REGBYTES 布局定位各字段，不能自行假设另一套数据布局。
- 通用寄存器恢复：x0 恒为零无需恢复；ra、gp、tp、t0–t6、s0–s11、a0–a7 从各自在 Trap frame 中的偏移用 LOAD 恢复到原寄存器。
- 栈指针恢复：SAVE_ALL 先把陷入前的 sp 暂存到 sscratch，再写入 2*REGBYTES 位置，RESTORE_ALL 必须从该位置恢复 sp，并且要把这一步安排在最后一次以 sp 为基址访问内存之后。
- CSR 恢复：从 32*REGBYTES 和 33*REGBYTES 分别恢复 sstatus 和 sepc；sret 依赖 sstatus 中的 SPP、SPIE 以及 sepc 完成特权级、中断使能和返回地址的恢复，scause 与 sbadaddr 不需要写回。
- 临时寄存器使用：恢复过程中若使用某个通用寄存器作为临时寄存器，必须在该寄存器被恢复之前完成使用，不能把尚未恢复的现场值覆盖掉。
- 入口调用：__alltraps 在 SAVE_ALL 之后把当前 sp 放入 a0，再调用 trap；调用前后 sp 必须保持不变，使 trap() 收到指向完整 Trap frame 的 struct trapframe *。
- 返回衔接：trap() 返回后不得重新保存上下文，应直接进入 __trapret，由 RESTORE_ALL 和 sret 完成返回。

模块完成后，应该保证：

- 调用 trap() 时 a0 等于 SAVE_ALL 完成后的 sp，所指向的 Trap frame 完整覆盖 0 到 35*REGBYTES 的字段。
- trap() 返回后 sp 与调用前相同，__trapret 仍能以同一个 Trap frame 为基址执行 RESTORE_ALL。
- 31 个可写通用寄存器全部恢复为陷入前的值，包括 test_lab3_context.S 检查的 t0–t6、s0–s11、a1–a7，且 x0 保持为 0。
- sp 恢复为陷入前的栈指针，Trap frame 占用的 36*REGBYTES 被完整弹出，返回后不残留额外栈占用。
- sepc 从 Trap frame 恢复，trap() 对 tf->epc 的修改不会被旧值覆盖。
- sret 执行后按恢复的 sepc 继续原执行流，ra 恢复为陷入前的返回地址，不残留 trap() 的调用链。
- 与正确的 trap() 配合时，Test 1 输出 “[Test 1] Trap entry and breakpoint return: PASS (35/35)”。

### 3.3 任务二：中断与异常处理

对应文件：`kern/trap/trap.c`。提交的提示词如下：

在 kern/trap/trap.c 的 lab3 function2 YOUR CODE 处实现 void interrupt_handler(struct trapframe *tf) 和 void exception_handler(struct trapframe *tf)，并在文件顶部添加 #include <sbi.h>；SHUTDOWN_AFTER 当前工程未定义，需在 TICK_NUM 附近补充（表示累计统计输出次数，取值大于 2，例如 10）。interrupt_handler 负责时钟中断、软件中断和未知中断，exception_handler 负责非法指令、断点和其他异常。只修改 trap.c，不修改 print_ticks、trap_dispatch、trap、idt_init，也不修改 trap.h、trapentry.S、init.c、测试文件和 Makefile；函数定义在 trap_dispatch 之前即可，无需前向声明。可使用 tf->cause、tf->epc、print_trapframe、print_ticks、clock_set_next_event、ticks、TICK_NUM、IRQ_S_TIMER、IRQ_S_SOFT、CAUSE_ILLEGAL_INSTRUCTION、CAUSE_BREAKPOINT、sbi_shutdown 和 cprintf。请直接修改真实文件并保存，不要只输出示例代码，时间复杂度不做要求。

这个模块需要处理以下场景：

- 中断号提取：清除 tf->cause 的最高位或取低 8 位后再比较，使带 bit 63 的软件中断也能正确识别。
- 软件中断：输出 “Supervisor software interrupt”。
- 时钟中断：每次调用 clock_set_next_event()，并让 ticks 自增一次。
- 统计与关机：ticks 为 TICK_NUM 整数倍时调用 print_ticks()；累计输出达到 SHUTDOWN_AFTER 次后调用 sbi_shutdown()，且关机不得早于 ticks=200。
- 未知中断：输出 “Unknown interrupt: cause %d” 并调用 print_trapframe(tf)。
- 断点异常：输出 “Breakpoint at 0x%lx”，再按指令长度推进 tf->epc。
- 非法指令：输出 “Illegal instruction at 0x%lx”，再按指令长度推进 tf->epc。
- 指令长度：读取 tf->epc 处的 16 位半字，低两位不为 0x3 时前进 2 字节，否则前进 4 字节，两类异常不共用固定偏移。
- 其他异常：调用 print_trapframe(tf) 兜底。

模块完成后，应该保证：

- 断点输出包含 “Breakpoint at”，tf->epc 前进 2 字节，Test 1 输出 PASS (35/35)。
- 非法指令输出包含 “Illegal instruction at”，tf->epc 前进 4 字节，Test 2 输出 PASS (25/25)。
- 输出包含 “Supervisor software interrupt”、“Unknown interrupt: cause 15” 和 “cause    0x00000014”。
- ticks 单调递增，每个 TICK_NUM 输出一次 “100 ticks”，ticks=200 时 Test 3 输出 PASS (40/40)。
- 每次时钟中断都重新设置了下一次事件，累计输出达到 SHUTDOWN_AFTER 次后关机且不早于 ticks=200。
- interrupt_handler 与 exception_handler 的函数签名不变，trap.c 中其他函数未被修改。

## 4. 实现与构建中的问题及处理

本节区分骨架缺失、实现细节和环境问题，避免把所有构建失败都归为 AI 生成代码错误。

### 4.1 Lab2 代码未完整迁移

原记录中的首次构建错误包括：

```text
kern/mm/best_fit_pmm.c:24:36: error: 'free_list' undeclared
kern/mm/best_fit_pmm.c:28:34: error: 'nr_free' undeclared
kern/mm/pmm.c:97:5: error: implicit declaration of function 'page_init'
```

Lab3 骨架中的 `best_fit_pmm.c` 和 `page_init()` 尚未补齐，因此需要迁移 Lab2 的相应实现。当前仍选择 `default_pmm_manager`，Best-Fit 文件参与编译，但未被选为运行时的物理内存管理器。

### 4.2 内存上界宏不兼容

迁移后出现 `PHYSICAL_MEMORY_END` 未定义。根据本工程的地址映射，改用：

```c
if (mem_end > (KERNTOP - PHYSICAL_MEMORY_OFFSET)) {
    mem_end = (KERNTOP - PHYSICAL_MEMORY_OFFSET);
}
```

原工程中该表达式对应 `0x88000000`，与原有物理内存上界一致。迁移代码时不仅要补齐函数，还要检查目标工程中的宏和地址约定。

### 4.3 断点不能固定跳过4字节

原记录使用反汇编确认了断点编码：

```text
6a: 9002        ebreak
6c: 10500513    li a0,261
```

地址从 `0x6a` 到 `0x6c`，说明该断点只有2字节。修正方式是按本实验涉及的指令编码判断长度，再推进 `tf->epc`，而非对两类异常统一加4。

### 4.4 关机条件与测试时机冲突

工程未定义 `SHUTDOWN_AFTER`，因此补充：

```c
#define SHUTDOWN_AFTER 10
```

该值统计的是 `print_ticks()` 的调用次数。若在 ticks=200 的中断处理过程中直接关机，主循环可能还未获得机会输出测试结果。因此设为10，让测试先输出 PASS，再在后续计时中关机。

### 4.5 构建位置与工具链环境

原记录还包括两项环境修正：进入包含 Makefile 的 `lab3_new` 目录执行构建；在 WSL 中使用 Linux 版工具链，并将其目录加入 PATH。具体命令见第7节。

## 5. 实验与操作系统原理的联系

| 知识点 | 本实验中的体现 | 与完整操作系统的区别 |
| --- | --- | --- |
| 中断与异常 | 共用汇编入口，在 C 层按原因分派 | 尚未实现完整设备中断与异常处置策略 |
| 现场保护 | Trap frame 保存通用寄存器和必要 CSR | 此处用于 Trap 返回，尚未实现进程切换 |
| 特权级与返回 | 通过 SPP、SPIE 和 sepc 控制 sret | 测试主体在 S 模式，没有用户态返回路径 |
| ABI | a0 传指针，C 函数返回时恢复 sp | 汇编必须额外保护被中断程序的完整现场 |
| 定时器 | 每次处理后设置下一次事件 | 仅计数与关机，尚未驱动调度器 |
| 指令编码 | 根据2/4字节长度推进异常返回地址 | 未实现完整变长解码与非法指令模拟 |

本实验最重要的认识是：**硬件只完成进入 Trap 所需的状态更新，软件仍须保证现场完整、调用关系正确、返回地址有效。** 三者中任何一项出错，都可能使处理程序执行完毕后无法正确返回。

## 6. 本实验的适用范围与未实现部分

- **用户态与内核栈切换**：当前入口直接使用内核栈，未实现用户态 Trap 及栈切换。
- **嵌套 Trap**：SIE 清零可屏蔽相应的 S 模式中断，但处理程序仍可能触发同步异常。本实验未实现完整的嵌套处理策略。
- **缺页与系统调用**：未实现按需分页、页面换入换出及 U 模式系统调用路径；SBI 调用用于请求更高特权级服务。
- **进程调度、多核与设备中断**：未实现调度器、多 hart 协同及 PLIC 等外部中断处理。
- **异常恢复与计时校准**：非法指令仅按实验要求跳过；计时间隔依赖既定环境，未实现完整的频率适配。

## 7. 构建、运行与评分验证记录

### 7.1 环境

| 项目   | 路径或版本                                                                                |
| ---- | ------------------------------------------------------------------------------------ |
| 工程   | `/mnt/d/OS-Lab/Lab3/lab3_new`                                                        |
| 工具链  | `/mnt/d/OS/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin` |
| GCC  | `riscv64-unknown-elf-gcc 10.2.0`（SiFive GCC-Metal）                                   |
| QEMU | `qemu-system-riscv64 4.1.1`                                                          |
| make | GNU Make 4.4.1                                                                       |
| 运行环境 | WSL Ubuntu                                                                           |

### 7.2 构建与启动

```bash
export PATH=/mnt/d/OS/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:$PATH
cd /mnt/d/OS-Lab/Lab3/lab3_new
make
make qemu
```

`make` 结束后生成 `bin/kernel` 和 `bin/ucore.img`。QEMU 启动后的关键输出：

```text
OpenSBI v0.4
...
(THU.CST) os is loading ...
Special kernel symbols:
  entry  0xffffffffc0200054 (virtual)
  etext  0xffffffffc0201dca (virtual)
  edata  0xffffffffc0206028 (virtual)
  end    0xffffffffc02064a0 (virtual)
Kernel executable memory footprint: 26KB
memory management: default_pmm_manager
check_alloc_page() succeeded!
satp virtual address: 0xffffffffc0205000
satp physical address: 0x0000000080205000
++ setup timer interrupts
100 ticks
100 ticks
...
```

说明内核已经完成 DTB 解析、物理内存管理自检、`stvec` 设置和定时器初始化，并开始周期性处理时钟中断。

### 7.3 评分

```bash
make grade
```

`make grade` 会先清理构建产物，把工程复制到 `/tmp/lab3_grade`，由 `tools/inject.py` 注入测试，再用 `tools/grade.sh` 分别编译运行三个用例。原始 `kern/init/init.c` 不会被修改。原实验记录中的评分输出如下：

```text
Building trap entry, register restore, and breakpoint EPC...
Running trap entry, register restore, and breakpoint EPC...
  - trap entry, register restore, and breakpoint EPC: PASS (35/35)
Building illegal instruction EPC advance and return...
Running illegal instruction EPC advance and return...
  - illegal instruction EPC advance and return: PASS (25/25)
Building re-armed supervisor timer and repeated trap return...
Running re-armed supervisor timer and repeated trap return...
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

三个用例分别验证：

1. Trap 入口、测试覆盖的通用寄存器恢复，以及断点处理后 `sepc` 指向 `c.ebreak` 的下一条指令；
2. 非法指令处理后 `epc` 前进 4 字节，并验证软件中断提示、未知中断提示和 `print_trapframe` 的兜底输出；
3. 时钟中断持续重新设置、`ticks` 持续递增，并在 `ticks=200` 时输出 PASS。

### 7.4 结论

原实验记录显示，三个评分用例均通过，总分为100/100。在现有测试覆盖的范围内，Trap 入口、现场恢复、异常返回地址推进和定时器重设能够协同工作。

这一结果不等于所有 Trap 场景均已验证。用户态切换、处理程序中的再次异常、更长指令编码、多核和设备中断仍在本实验实现范围之外。

## 参考资料

1. RISC-V International，[Supervisor-Level ISA](https://docs.riscv.org/reference/isa/priv/supervisor.html)：Trap 相关 CSR 与 S 模式机制。
2. RISC-V International，[Unprivileged ISA Introduction](https://docs.riscv.org/reference/isa/v20250508/unpriv/intro.html)：指令长度编码与非法指令约定。
3. 本实验工程：`kern/trap/trapentry.S`、`kern/trap/trap.c`、`kern/trap/trap.h`、`tools/inject.py`、`tools/grade.sh`；Lab2 内存管理实现。
