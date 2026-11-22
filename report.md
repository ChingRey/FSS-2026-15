# 实验四：虚拟内存页操作、进程创建与进程调度——实验报告

> 整理日期：2026-11-22。报告依据当前 `lab4_new` 工作区的实际代码、三个任务提示词、`tools/test_lab4.c`、`tools/inject.py`、`tools/grade.sh` 以及 `make qemu`、`make grade` 的实际输出整理。实验四没有把页表、进程和调度看成三个互不相关的练习，而是按照“地址映射可维护—执行实体可创建—CPU 执行流可切换”的次序组织。

## 材料来源与分工

| 成员 | 学号 | 负责的功能模块 | 主要工作与验证 |
| --- | --- | --- | --- |
| 李树青 | 2512885 | 功能模块一：虚拟内存页操作 | 实现 `page_insert`、`page_remove`，维护页表项、物理页引用计数和 TLB，并完成 PMM 测试验证 |
| 胡博延 | 2513387 | 功能模块二：进程创建及初始化 | 实现 `alloc_proc`、`proc_init`、`kernel_thread`、`do_fork`、`copy_thread`、`copy_mm`，并完成进程创建测试验证 |
| 杨永琛 | 2513097 | 功能模块三：进程调度 | 实现 `cpu_idle`、`schedule`、`proc_run`、`switch_to`，完成 Sv39 页表切换和上下文切换，并完成调度测试验证 |

三个成员分别负责一个完整功能模块。公共的 Lab2/Lab3 底层代码迁移、三个模块的接口联调、`make qemu` 启动检查和 `make grade` 最终评分由三人共同完成。

## 1. 本章节整体逻辑理解

### 1.1 逻辑主线

Lab2 建立了物理页框和连续页分配的基础，Lab3 建立了 Trap 现场的保存、处理和恢复机制。Lab4 在这两层基础上继续向上推进，形成两条相互依赖的主线：

1. 页面映射能力：在已有物理页管理之上维护 Sv39 多级页表，使线性地址能够稳定映射到物理页；
2. 执行流管理能力：建立 PCB、内核线程、首次运行现场和上下文切换机制，让 CPU 可以从 `idleproc` 切换到 `initproc` 及其他内核线程。

整体主线可以概括为：

```text
物理页管理
   ↓
Sv39 页表与 page_insert/page_remove
   ↓
PCB、内核栈、trapframe、context
   ↓
idleproc / initproc / kernel_thread
   ↓
可运行队列、schedule、proc_run、switch_to
   ↓
多个内核线程轮流执行
```

三个功能模块之间不是简单的调用顺序，而是共享同一套状态：

- `page_insert` 和 `page_remove` 决定某个线性地址最终映射到哪一页物理页；
- `copy_thread` 把新的 trapframe 放入内核栈，为线程首次运行准备现场；
- `proc_run` 切换 `satp` 和 TLB，再由 `switch_to` 保存、恢复 `context`；
- 调度器选择目标进程后，执行流通过 `forkret → forkrets → __trapret → kernel_thread_entry` 进入线程函数；
- 当内核线程函数返回时，入口代码调用 `do_exit` 收尾。

### 1.2 构建与启动顺序

构建系统编译 `kern/` 和 `libs/` 下的 `.c`、`.S` 文件，使用 `tools/kernel.ld` 链接到虚拟地址 `0xFFFFFFFFC0200000`，最后由 `objcopy` 生成 `bin/ucore.img`。QEMU 把镜像加载到物理地址 `0x80200000`。

当前 Lab4 的启动流程为：

```text
kern_entry
  → 保存 hartid、DTB 地址
  → 建立 boot_page_table_sv39
  → 设置 satp，刷新 TLB
  → 设置 bootstack
  → kern_init
       → 清零 BSS
       → dtb_init
       → cons_init、打印内核信息
       → pmm_init
            → 物理页管理初始化
            → boot_pgdir 设置与页表检查
            → kmalloc_init
       → pic_init、idt_init
       → vmm_init
       → proc_init
       → clock_init
       → intr_enable
       → cpu_idle
```

`pmm_init` 在 Lab4 中不仅初始化物理页分配器，还设置 `boot_pgdir_va` 和 `boot_pgdir_pa`，执行 `check_pgdir`、`check_boot_pgdir`，随后建立 SLOB 堆分配器。只有这些步骤完成后，`proc_init` 才能使用 `kmalloc` 创建 PCB，并使用 `alloc_pages` 分配线程内核栈。

## 2. 核心函数与功能模块理解

### 2.1 Sv39 页表结构与 get_pte

Sv39 把一个 39 位虚拟地址划分为：

```text
9 位 PDX1 + 9 位 PDX0 + 9 位 PTX + 12 位页内偏移
   VPN[2]      VPN[1]      VPN[0]
```

对应宏为：

```c
PDX1(la)  = (la >> 30) & 0x1FF;
PDX0(la)  = (la >> 21) & 0x1FF;
PTX(la)   = (la >> 12) & 0x1FF;
PGOFF(la) = la & 0xFFF;
```

每级页表有 512 项。页表项保存的是下一级页表或最终物理页的物理页号，不是可直接访问的虚拟地址。

`get_pte(pgdir, la, create)` 的查找过程是：

1. 用 `PDX1(la)` 在根页表中找到中间级页表项；
2. 如果该项无效且 `create == 1`，分配一个物理页，清零并将其地址写入父级页表项；
3. 用 `PDE_ADDR` 和 `KADDR` 把下一级页表的物理地址转换为内核可访问的虚拟地址；
4. 用 `PDX0(la)` 找到底层页表项，必要时再次分配；
5. 用 `PTX(la)` 返回最终 PTE 的内核虚拟地址。

`PTE_ADDR(pte)` 会清除低 10 位权限字段，再把 PPN 从页表项的第 10 位移到物理地址的第 12 位，从而得到物理页基址。`PDE_ADDR` 与它语义相同，只是强调该地址指向下一级页表。

### 2.2 page_insert 与 page_remove

#### page_insert

`page_insert` 负责建立或更新映射。其核心步骤是：

1. 使用 `get_pte(pgdir, la, 1)` 找到目标 PTE，必要时创建中间页表；
2. 如果找不到 PTE，返回 `-E_NO_MEM`；
3. 如果原地址已经映射到其他物理页，先调用 `page_remove` 撤销旧映射；
4. 如果原地址已经映射到同一个物理页，只更新权限，不增加引用计数；
5. 新映射通过 `pte_create(page2ppn(page), perm)` 写入；
6. 调用 `tlb_invalidate`，让旧翻译不再被 CPU 使用。

`page_insert` 最容易出错的是引用计数。不能无条件先增加新页引用计数，否则在同一地址反复修改权限时，物理页引用计数会持续增长。当前实现将“同一页更新权限”和“建立新映射”分开处理，保证同一映射的引用计数保持不变。

另外，`get_pte` 可能已经分配了高级页表页，随后却因下一级页表分配失败而返回空指针。当前实现用 `created_top` 记录本次是否新建了高级页表；失败时撤销这个半完成的中间页表项并释放对应物理页，避免异常失败路径留下孤立页表。

#### page_remove

`page_remove` 是幂等接口：

- PTE 不存在或无效时直接返回；
- 有效映射先清空 PTE；
- 调用 `tlb_invalidate` 使该地址的 TLB 翻译失效；
- 物理页引用计数减一；
- 减到 0 时调用 `free_page`。

页表项、引用计数和 TLB 三者必须同步。只清 PTE 而不刷新 TLB，会出现页表已经改变、CPU 仍使用旧翻译的现象；只减少引用计数而不清 PTE，则会留下悬空映射。

### 2.3 PCB 的字段与进程状态

`struct proc_struct` 是进程/线程管理核心：

| 字段 | 含义 |
| --- | --- |
| `state` | 进程状态 |
| `pid` | 进程标识 |
| `runs` | 被调度执行次数 |
| `kstack` | 内核栈基址 |
| `need_resched` | 是否需要重新调度 |
| `parent` | 父进程 |
| `mm` | 地址空间管理结构 |
| `context` | 切换时需要保存的寄存器上下文 |
| `tf` | 内核栈上的 trapframe |
| `pgdir` | 页目录物理地址 |
| `flags` | 进程标志 |
| `name` | 进程名称 |
| `list_link` | 可运行队列链表节点 |
| `hash_link` | PID 哈希表节点 |

进程状态的主路径是：

```text
PROC_UNINIT
    ↓ wakeup_proc
PROC_RUNNABLE
    ↓ schedule / proc_run
运行中
    ↓ 阻塞
PROC_SLEEPING
    ↓ wakeup_proc
PROC_RUNNABLE
```

本实验主要使用 `PROC_UNINIT`、`PROC_RUNNABLE`，并为后续休眠和退出保留状态。

### 2.4 alloc_proc

`alloc_proc` 使用 `kmalloc` 分配 PCB，然后把 PCB 置于安全空白状态：

- `state = PROC_UNINIT`；
- `pid = -1`；
- `runs = 0`；
- `kstack = 0`；
- `need_resched = 0`；
- `parent = NULL`；
- `mm = NULL`；
- `tf = NULL`；
- `pgdir = 0`；
- `flags = 0`；
- `context` 清零；
- 初始化 `list_link` 和 `hash_link`。

如果 PCB 字段没有初始化，后续调度线程可能把未定义状态误判为可运行，或者在没有内核栈的情况下进入上下文切换。

### 2.5 proc_init

`proc_init` 建立系统第一组关键线程：

1. 初始化 `proc_list` 和 1024 个 PID 哈希桶；
2. 创建 `idleproc`，PID 为 0；
3. 把 `idleproc->kstack` 设置为现有 `bootstack`；
4. 设置 `idleproc->pgdir = boot_pgdir_pa`；
5. 设置 `need_resched = 1`，使第一次进入 `cpu_idle` 时会触发调度；
6. 设置 `current = idleproc`；
7. 调用 `kernel_thread(init_main, "Hello world!!", 0)` 创建 `initproc`；
8. 通过返回的 PID 在哈希表中找到 `initproc`，将其命名为 `init`。

`idleproc` 不是普通 fork 出来的线程。它复用当前启动执行现场和 `bootstack`，没有自己的 trapframe，也没有普通父进程。`initproc` 则是正常通过 `do_fork` 创建的第一个内核线程。

### 2.6 kernel_thread

`kernel_thread` 把普通 C 函数包装为线程入口。它建立临时 trapframe，并设置：

```c
tf.gpr.s0 = (uintptr_t)fn;
tf.gpr.s1 = (uintptr_t)arg;
tf.epc    = (uintptr_t)kernel_thread_entry;
```

随后调用：

```c
return do_fork(clone_flags, 0, &tf);
```

`kernel_thread_entry` 中执行：

```asm
move a0, s1
jalr s0
```

因此 `s0` 保存线程函数地址，`s1` 保存函数参数。选择这两个 callee-saved 寄存器，可以避免在 sret 之后、进入目标函数之前丢失函数和参数。

### 2.7 do_fork 与资源管理

`do_fork` 把 PCB、内核栈、地址空间和首次运行现场组合成一个可运行线程：

```text
alloc_proc
  → setup_kstack
  → copy_mm
  → copy_thread
  → get_pid
  → hash_proc
  → list_add(proc_list)
  → nr_process++
  → wakeup_proc
```

失败路径同样重要：

- PCB 分配失败：直接返回 `-E_NO_MEM`；
- 内核栈分配失败：释放 PCB；
- `copy_mm` 失败：释放内核栈和 PCB；
- 成功登记后再将进程设为 `PROC_RUNNABLE`。

进程在尚未完整建立时不能加入可运行队列，否则调度器可能选到一个没有内核栈或没有 trapframe 的半成品 PCB。`do_fork` 用中断开关构成临界区，避免链表、PID 哈希表和进程计数被中断路径并发修改。

### 2.8 copy_thread 与首次运行现场

`copy_thread` 把 trapframe 放在新线程内核栈顶部：

```c
proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE) - 1;
```

随后：

- 复制调用者提供的 trapframe，或建立默认现场；
- 设置 `tf->gpr.sp`；
- 设置 `tf->status` 的 `SPP` 和 `SPIE`；
- 设置 `context.ra = forkret`；
- 设置 `context.sp = proc->tf`。

`SPP = 1` 保证 `sret` 返回 S 模式；`SPIE = 1` 保证返回线程后打开中断。如果只清零 `status`，首次 `sret` 会回到错误的特权级或中断状态。

新线程第一次运行链路为：

```text
proc_run
  → switch_to
  → forkret
  → forkrets(proc->tf)
  → __trapret
  → kernel_thread_entry
  → 用户指定的内核线程函数
```

这也体现了 Lab3 与 Lab4 的衔接：Lab3 的 `__trapret` 负责恢复完整的通用寄存器和 CSR，Lab4 把新线程的初始现场装入同一个 trapframe。

### 2.9 copy_mm

本阶段的 `copy_mm` 是地址空间处理接口的占位实现：

```c
proc->mm = (clone_flags & CLONE_VM) ? current->mm : NULL;
return 0;
```

它暂时不复制 VMA、页目录或物理页。由于当前创建的都是内核线程，它们共享内核页表，`copy_thread` 和 `do_fork` 为新进程复制 `current->pgdir`，因此简单内核线程可以直接运行。真正的用户地址空间复制留待后续实验。

### 2.10 schedule

`schedule` 在关闭中断的临界区内完成一次轮转选择：

1. 清除当前进程的 `need_resched`；
2. 如果当前是 `idleproc`，从 `proc_list` 头部开始；
3. 如果当前是普通线程，从当前线程的链表节点之后开始；
4. 环形扫描所有进程，选择状态为 `PROC_RUNNABLE` 的线程；
5. 找不到可运行线程时选择 `idleproc`；
6. 增加目标线程的 `runs`；
7. 目标不同则调用 `proc_run`。

采用“从当前节点的下一个节点开始”可以避免当前线程一直优先选中自己，形成简单的轮转调度。

### 2.11 proc_run

`proc_run` 必须在关闭中断的条件下完成：

1. 保存旧 `current`；
2. 把 `current` 更新为目标进程；
3. 将目标进程的页目录物理地址写入 `satp`；
4. 刷新 TLB；
5. 调用 `switch_to` 切换上下文。

Sv39 的 `satp` 写入方式为：

```c
write_csr(satp,
          ((uintptr_t)SATP_MODE_SV39 << 60) |
          (proc->pgdir >> RISCV_PGSHIFT));
```

这里不能使用 `libs/riscv.h` 中旧的 `lsatp()`，因为它面向 Sv32，无法正确表示 Sv39 的 64 位 `satp`。

### 2.12 switch_to

`struct context` 保存 14 个上下文字段：

```text
ra, sp, s0, s1, s2, ..., s11
```

`switch_to` 先按固定偏移把当前寄存器写入 `from`，再从 `to` 中恢复它们。RV64 下每个字段 8 字节，偏移依次为 0、8、16，直到 104。

函数最后恢复 `ra`，通过 `ret` 跳到目标上下文原来保存的返回地址。因此 `proc_run` 中的 `switch_to` 返回时，已经运行在新线程的栈和寄存器环境中。

### 2.13 cpu_idle

`cpu_idle` 是 `idleproc` 的主体：

```c
while (1) {
    if (current->need_resched) {
        schedule();
    }
    asm volatile("wfi");
}
```

只有在存在调度需求时进入调度器；没有可运行线程时停留在低功耗等待状态。

## 3. 各练习的 SPEC 与提示词

### 3.1 提示词编写过程

先阅读 Lab4 源码、`struct Page`、`struct proc_struct`、`struct context`、`struct trapframe`、Lab3 的 Trap 返回链路以及 `tools/test_lab4.c`，明确每个函数的输入、输出、失败路径和可使用的现有接口。随后把模块拆成三个提示词：

- 第一个只处理页表映射；
- 第二个只处理进程创建与初始化；
- 第三个只处理调度和上下文切换。

每个提示词都写明“只改哪些函数”“不得修改哪些公共结构”“资源失败时应如何清理”“TLB、中断和引用计数如何同步”，避免 AI 直接改变测试框架或自行设计另一套接口。

### 3.2 任务一：虚拟内存页操作

对应函数：

```text
kern/mm/pmm.c
├─ page_insert
└─ page_remove
```

提交的提示词如下：

```text
请完成 ucore RISC-V Lab4 的“虚拟内存页操作”模块。

只修改命令：
kern/mm/pmm.c

只实现以下两个函数，不要修改其他函数：
1. int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm);
2. void page_remove(pde_t *pgdir, uintptr_t la);

page_insert：
- 使用 get_pte(pgdir, la, 1) 获取目标 PTE；
- get_pte 失败时返回 -E_NO_MEM，不能留下半完成的映射；
- 地址无映射时增加物理页引用计数并写入 PTE；
- 已映射到同一物理页时只更新权限，不重复增加引用计数；
- 已映射到其他物理页时，先安全删除旧映射，再建立新映射；
- 使用 pte_create(page2ppn(page), perm) 创建 PTE；
- 更新后调用 tlb_invalidate。

page_remove：
- 使用 get_pte(pgdir, la, 0) 查找 PTE；
- 无映射时安静返回，函数必须幂等；
- 删除时清空 PTE、刷新 TLB、减少引用计数；
- 引用计数降到 0 时调用 free_page。

请结合 get_pte、pte2page、page_ref_inc、page_ref_dec、pde2page、
free_page、pte_create 和 tlb_invalidate 的现有定义实现。
不要新增全局变量，不要改变函数签名，不要修改头文件、Makefile
或其他函数。
```

模块完成条件：

- 同一地址反复映射同一页时，引用计数保持正确；
- 替换映射时旧页引用计数正确下降；
- 删除不存在的映射不会断言或崩溃；
- 页表项、引用计数和 TLB 状态保持一致；
- `Test PMM` 通过。

### 3.3 任务二：进程创建及初始化

对应函数：

```text
kern/process/proc.c
├─ alloc_proc
├─ proc_init
├─ kernel_thread
├─ do_fork
├─ copy_thread
└─ copy_mm
```

提交的提示词如下：

```text
请完成 ucore RISC-V Lab4 的“进程创建及初始化”模块。

只修改 kern/process/proc.c，只实现 alloc_proc、proc_init、
kernel_thread、do_fork、copy_thread、copy_mm 六个函数。

alloc_proc：
- 使用 kmalloc 分配 PCB；
- 完整初始化状态、PID、运行次数、内核栈、调度标记、父进程、
  地址空间、context、trapframe、页目录、flags 和链表节点；
- 状态设为 PROC_UNINIT，pid 设为 -1。

proc_init：
- 初始化进程链表和所有 PID 哈希桶；
- 把当前执行现场包装成 idleproc，pid=0，kstack=bootstack；
- 设置 idleproc 的页目录、名称和首次调度标记；
- 设置 current=idleproc；
- 通过 kernel_thread 创建 initproc，并设置名称为 init。

kernel_thread：
- 建立临时 trapframe；
- 把函数入口放到 gpr.s0，参数放到 gpr.s1；
- 把 epc 设置为 kernel_thread_entry；
- 调用 do_fork。

do_fork：
- 依次完成 PCB、内核栈、地址空间、首次运行现场和 PID；
- 加入 proc_list 和 PID 哈希表；
- 更新进程数并唤醒新线程；
- 每个失败路径都要释放已经申请的资源；
- 在关闭中断的临界区内维护进程管理结构。

copy_thread：
- 在新内核栈顶部建立 trapframe；
- 设置首次运行的 sp、status 和 epc；
- context.ra 设置为 forkret，context.sp 指向 trapframe；
- 保证 switch_to → forkret → forkrets → __trapret →
  kernel_thread_entry 的首次运行链路。

copy_mm：
- 本阶段只做占位处理；
- CLONE_VM 时共享 current->mm，否则设为 NULL；
- 不实现真正的用户地址空间复制。

不要修改 do_exit、init_main、set_proc_name、get_pid 等已有函数，
不要修改 proc.h、Makefile 或测试文件。
```

模块完成条件：

- `idleproc->pid == 0`，其内核栈等于 `bootstack`；
- `initproc->pid == 1`，状态为 `PROC_RUNNABLE`，父进程为 `idleproc`；
- 新线程的 `context.ra` 指向 `forkret`；
- `context.sp` 和 trapframe 位于新线程自己的内核栈；
- fork 任一步失败都不会留下未释放的 PCB 或内核栈；
- `Test Process Create` 通过。

### 3.4 任务三：进程调度

对应函数：

```text
kern/process/proc.c
├─ cpu_idle
└─ proc_run

kern/schedule/sched.c
└─ schedule

kern/process/switch.S
└─ switch_to
```

提交的提示词如下：

```text
请完成 ucore RISC-V Lab4 的“进程调度”模块。

只实现 cpu_idle、proc_run、schedule、switch_to 四个函数。

cpu_idle：
- 无限循环；
- current->need_resched 为真时调用 schedule；
- 没有调度需求时等待事件。

schedule：
- 使用 local_intr_save/local_intr_restore 保持原子性；
- 清除当前进程的 need_resched；
- 从 proc_list 环形查找 PROC_RUNNABLE 线程；
- 找不到时回退到 idleproc；
- 更新 runs；
- 目标不同则调用 proc_run。

proc_run：
- 在关闭中断的临界区内更新 current；
- 使用 Sv39 模式设置 satp；
- 刷新 TLB；
- 调用 switch_to 保存旧 context、恢复新 context；
- 不能使用面向 Sv32 的 lsatp。

switch_to：
- 按 struct context 的字段保存、恢复 ra、sp、s0-s11；
- 共 14 个寄存器；
- 最后通过恢复后的 ra 返回。

不要改变结构体布局、测试文件、wakeup_proc 或 Makefile。
```

模块完成条件：

- 空闲时能够从 `idleproc` 切换到 `initproc`；
- 多个可运行内核线程能够轮流获得执行机会；
- 切换期间中断被屏蔽，当前进程、页表和寄存器现场保持一致；
- `schedule` 可以正确处理“没有可运行线程”的情况；
- `Test Schedule` 通过。

## 4. AI 生成代码的问题与处理

### 4.1 Lab4 骨架缺少 Lab2/Lab3 的完整底层实现

Lab4 新增了进程、调度、VMM 和堆分配，但初始工程中的 `default_pmm.c` 只有检查代码，`pmm.c` 缺少 `page_init()`，陷阱处理也保留了部分占位内容。如果直接实现 Lab4 目标函数，工程仍可能无法链接或无法启动。

处理方式是以 `lab4_new` 为主工程，从 `lab3_new` 原样接入以下已完成内容：

- 完整的 `default_pmm_manager`；
- `page_init()`；
- `interrupt_handler()` 和 `exception_handler()`；
- Trap 返回所需的 `RESTORE_ALL` 和 `trap()` 调用。

没有用 Lab3 的 `Makefile`、`init.c`、`proc.c` 或整个目录覆盖 Lab4，因为 Lab4 的初始化顺序需要调用 `vmm_init`、`proc_init` 和 `cpu_idle`。

### 4.2 缺失 sbi.h 导致链接失败

第一次完整构建时出现：

```text
kern/trap/trap.c: implicit declaration of function 'sbi_shutdown'
riscv64-unknown-elf-ld: undefined reference to 'sbi_shutdown'
```

`lab4_new/libs/sbi.h` 本身已经提供了 `sbi_shutdown()` 的静态内联实现，问题只是 `trap.c` 没有包含该头文件。在 `trap.c` 顶部加入：

```c
#include <sbi.h>
```

后链接通过。处理时没有复制 Lab3 的 `sbi.c`，避免同一组 SBI 接口出现两套实现。

### 4.3 page_insert 同一页重复映射导致引用计数增加

如果采用“先无条件 `page_ref_inc(new_page)`，再处理旧 PTE”的写法，那么当同一地址已经映射到同一物理页、仅修改权限时，引用计数会从 1 变成 2。测试中随后断言 `page_ref(p3) == 1` 会失败。

当前实现先判断旧物理页是否就是目标页：

- 同一页：只更新 PTE 权限和 TLB，不改变引用计数；
- 不同页：先调用 `page_remove` 撤销旧页，再为新页增加引用计数。

这说明 `page_insert` 不是简单的“覆盖 PTE”，而是对映射关系的更新。

### 4.4 映射失败后的页表清理

`get_pte(..., 1)` 可能在创建高级页表后，因为下一级页表分配失败而返回空指针。当前 `page_insert` 在调用前记录高级页表项是否原本无效：

```c
bool created_top = !(pgdir[PDX1(la)] & PTE_V);
```

如果 `get_pte` 失败，而该项已经在本次调用中变成有效，则撤销这个中间页表项并释放对应页表页。这样失败路径不会留下半完成的映射或孤立页表。

### 4.5 copy_thread 的 status 设置

最初只复制 trapframe 时，临时 trapframe 的 `status` 为 0。RISC-V 的 `sret` 会依据 `SPP`、`SPIE` 决定返回特权级和中断使能状态，因此新线程可能无法正确返回 S 模式。

当前实现设置：

```c
proc->tf->status =
    (read_csr(sstatus) & ~SSTATUS_SPP)
    | SSTATUS_SPP
    | SSTATUS_SPIE;
```

它保证首次运行返回 S 模式，并在进入线程后打开中断。

### 4.6 idleproc 的首次调度

`proc_init` 创建 `initproc` 后，`current` 仍然是 `idleproc`。如果 `idleproc->need_resched` 为 0，`cpu_idle` 会一直等待，`initproc` 不会被执行。

当前实现在 `proc_init` 中设置 `idleproc->need_resched = 1`，因此第一次执行 `cpu_idle` 时就会进入 `schedule`，选择状态为 `PROC_RUNNABLE` 的 `initproc`。

### 4.7 process exit panic 并非本实验错误

`make qemu` 正常启动并执行 `init_main` 后，输出：

```text
this initproc, pid = 1, name = "init"
To U: "Hello world!!".
To U: "en.., Bye, Bye. :)"
kernel panic at kern/process/proc.c:208:
    process exit!!.
```

`init_main` 返回后，`kernel_thread_entry` 会调用 `do_exit`。本实验任务没有要求实现进程退出和资源回收，因此 `do_exit` 仍保留为 `panic` 占位。上述输出说明进程创建、首次运行和基本调度链已经建立，正式评分会通过注入测试检查三个模块。

### 4.8 编译警告

评分构建中还出现两类警告：

1. `kmalloc.c` 中 `kva2page` 的参数由整数转换为 `void *`；
2. 测试线程函数返回 `void`，而 `kernel_thread` 期望 `int (*)(void *)`。

这两个问题分别属于公共代码的指针类型写法和测试函数的函数指针类型，不影响当前 Lab4 的执行和评分。实际 `make grade` 输出为三项 `OK`，总分 `30/30`。

## 5. 实验重要知识点与 OS 原理的对应

| 知识点 | 实验中的含义 | 与 OS 原理的联系及差异 |
| --- | --- | --- |
| 多级页表 | `PDX1`、`PDX0`、`PTX` 分别索引三级页表 | 原理中用多级页表节省页表空间；本实验仅实现内核线程使用的基础映射 |
| 页表项编码 | PTE 中保存 PPN 和权限位，`PTE_ADDR` 取出物理页基址 | 原理中 MMU 自动完成地址翻译；本实验需要软件维护 PTE 和 TLB |
| 映射更新 | `page_insert` 区分同一页和其他页 | 原理中页表是地址空间描述；映射更新必须保持映射与引用计数一致 |
| TLB | `sfence.vma` 使旧翻译失效 | 原理中 TLB 是 MMU 缓存；页表修改后必须显式刷新 |
| 物理页引用计数 | 多个地址可映射同一物理页 | 原理中共享页需要引用计数；本实验通过 `page_ref_inc/dec` 实现 |
| PCB | `struct proc_struct` 描述线程资源 | 原理中 PCB 是进程存在的基本表示；本实验线程与进程结构简化统一 |
| 内核栈 | `setup_kstack` 分配连续页 | 原理中内核态执行需要独立内核栈；本实验使用固定两页 |
| trapframe | 保存线程首次运行和 Trap 返回现场 | 原理中上下文块包括 PC 和寄存器；本实验用 `struct trapframe` 表示 |
| context | 保存 `ra`、`sp`、`s0-s11` | 原理中上下文切换保存 callee-saved 状态；临时寄存器不需要跨线程保存 |
| fork | 建立 PCB、内核栈、PID 和调度关系 | 原理中 fork 创建地址空间副本；本阶段 `copy_mm` 仅为占位 |
| 调度 | 环形扫描 `PROC_RUNNABLE` | 原理中调度器在可运行任务间分配 CPU；本实验采用简单轮转策略 |
| idle | 无可运行线程时回退 `idleproc` | 原理中 CPU 不应空转；本实验用 `wfi` 等待事件 |
| satp | `proc_run` 切换页目录 | 原理中每个地址空间对应一个页表根；本实验多数线程共享内核页表 |
| 首次运行链路 | `forkret → forkrets → __trapret` | 原理中新任务从指定入口开始执行；本实验复用 Trap 恢复机制建立初始现场 |

## 6. OS 原理中重要但本实验未实现的知识点

- **用户地址空间复制**：`copy_mm` 目前只保存 `mm` 指针或不设置，没有复制 VMA 和页表；
- **缺页异常与按需分页**：VMM 只完成 VMA 基础检查，没有完整页错误处理和页面调入；
- **进程退出与回收**：`do_exit` 仍为 `panic`，没有僵尸状态、父进程回收和资源销毁；
- **阻塞、睡眠和等待**：调度只识别 `PROC_RUNNABLE`，没有完整睡眠队列和唤醒等待；
- **复杂调度策略**：当前是简单轮转，没有优先级、时间片统计或公平调度；
- **进程隔离**：所有内核线程共享内核页表，没有用户态地址空间隔离；
- **写时复制**：没有实现 fork 后按页复制，也没有 COW 引用共享；
- **多核并发**：实验运行在单 hart，进程链表和页表操作没有完整自旋锁；
- **文件与系统调用**：没有完整文件描述符、用户态系统调用及设备管理；
- **页面换入换出**：没有交换空间、页面置换算法和完整缺页处理路径。

## 7. 构建、运行与评分验证记录

### 7.1 环境

| 项目 | 路径或说明 |
| --- | --- |
| 工程 | `Desktop/lab4 带提示词/lab4_new` |
| 工具链 | RISC-V 64 位 ELF 工具链 |
| 运行环境 | WSL Ubuntu |
| QEMU | `qemu-system-riscv64` |
| 评分入口 | `make grade` |

### 7.2 make qemu

构建完成后生成 `bin/kernel` 和 `bin/ucore.img`。QEMU 的关键输出为：

```text
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)
  End:  0x0000000087ffffff

memory management: default_pmm_manager
check_alloc_page() succeeded!
check_pgdir() succeeded!
check_boot_pgdir() succeeded!
use SLOB allocator
kmalloc_init() succeeded!
check_vma_struct() succeeded!
check_vmm() succeeded.
idleproc pid = 0, name = "idle"
initproc pid = 1, name = "init"
+ setup timer interrupts
this initproc, pid = 1, name = "init"
To U: "Hello world!!".
To U: "en.., Bye, Bye. :)"
```

这些输出依次证明：

- DTB 正确识别 128 MiB 物理内存；
- 物理页分配和页表检查成功；
- SLOB 堆分配器可用；
- VMA 基础检查成功；
- `idleproc` 和 `initproc` 创建成功；
- `initproc` 被实际调度并执行。

随后 `do_exit` 因当前阶段未实现而触发 `process exit!!` panic，原因已在 4.7 节说明。

### 7.3 make grade

运行：

```bash
make grade
```

评分脚本会：

1. 清理并复制工程到临时目录；
2. 由 `inject.py` 把 Lab4 测试复制到临时工程的 `kern/test`；
3. 修改临时副本的 Makefile 和测试注入点；
4. 在临时副本中重新构建并启动 QEMU；
5. 检查固定输出字符串，不污染原始工程。

实际结果：

```text
lab4 injection complete
  -check PMM:                                OK
  -check Process Create:                     OK
  -check Schedule:                           OK
Total Score: 30/30
```

三个评分点分别验证：

| 测试 | 主要检查内容 |
| --- | --- |
| PMM | 新映射、同一页权限更新、替换旧页、引用计数、幂等删除 |
| Process Create | PID、状态、`kstack`、`bootstack`、父进程、trapframe 和 context |
| Schedule | 创建多个内核线程，检查轮转调度后两个线程都实际运行 |

### 7.4 结论

Lab4 已经完成从物理页到可执行内核线程的基本闭环：

- `page_insert/page_remove` 建立了页表项、物理页引用计数和 TLB 的同步关系；
- `alloc_proc/fork` 建立了内核线程的控制块、内核栈、PID 和首次运行现场；
- `schedule/proc_run/switch_to` 让 CPU 能够在多个线程之间保存并恢复执行流；
- `make grade` 三项测试全部通过，总分 `30/30`。

实验结果说明，地址映射、进程创建和上下文切换之间已经形成可运行的链路，可以继续作为后续用户地址空间复制、缺页处理和进程退出回收的基础。
