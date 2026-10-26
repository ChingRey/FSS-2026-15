# 实验二：物理内存管理与页表——小组实验报告

> 整理日期：2026-10-25。第1、2部分基于胡博延原稿补充；第3部分保留各任务提示词及编写过程；第4部分记录组内验证与修复；第5、6部分整理原理对应；第7部分合并杨永琛的调试报告。

## 小组材料来源与分工

| 成员 | 学号 | 采用的材料 |
|---|---|---|
| 胡博延 | 2513387 | 任务一、二代码与提示词；整体逻辑与核心模块理解原稿 |
| 李树青 | 2512885 | 伙伴系统代码及实现、测试提示词；任务一边界修订、代码融合与报告补充 |
| 杨永琛 | 2513097 | 分支任务QEMU/GDB页表调试报告 |

报告采用胡博延的任务一、二材料与杨永琛的调试总结。第7部分的路径、版本与地址采用杨永琛的调试环境，属于其提交的观测记录；本次整理未重新运行双重GDB实验。

## 1. 本章节整体逻辑理解

### 1.1 逻辑主线

本实验在 Lab1 基础上把内核从物理地址空间迁到虚拟地址空间，并建立物理内存页的管理机制。内核链接在高虚拟地址、加载在低物理地址；`entry.S` 用预置页表把最高 1 GiB 虚拟地址映射到物理内存起始处，打开 Sv39 后以虚拟地址运行。进入 `kern_init` 后，内核清零 BSS、从设备树获取内存范围，再由 `pmm_init` 建立 `Page` 描述符和空闲页链表，最后通过自检确认分配器可用。整体主线是：切换地址空间、获取内存信息、管理物理页、验证分配算法。

### 1.2 构建与启动的顺序

构建时 Makefile 完成编译、链接和镜像转换，`kernel.ld` 以 `0xFFFFFFFFC0200000` 为基址并指定 `kern_entry`；QEMU 把生成的 `ucore.img` 加载到 `0x80200000`，因此同一份代码同时具有虚拟地址和物理地址，二者相差固定偏移。

启动时 OpenSBI 把 hartid 和 DTB 地址交给内核并跳到 `0x80200000`；`kern_entry` 保存参数、建立 `satp`、刷新 TLB，然后把 `sp` 和 PC 切换到虚拟地址并进入 `kern_init`。`kern_init` 依次完成 BSS 清零、`dtb_init`、控制台输出和 `pmm_init`，最后进入循环。

## 2. 核心函数与功能模块理解

### 2.1 启动页表与地址切换

`entry.S` 的 `boot_page_table_sv39` 只有一个有效页表项，把虚拟地址最高 1 GiB 映射到物理 `0x80000000` 起的 1 GiB 窗口，其中实际有内存的是前 128 MiB。打开分页前代码按物理地址执行，因此计算页表物理页号时要减去虚实偏移；写入 `satp` 后，`sp` 和 PC 都改用虚拟地址，此后内核符号地址都是链接地址。这张页表只完成内核启动所需的直接映射，不涉及动态建立映射。

### 2.2 设备树探测与页描述符布局

`dtb_init` 把 `boot_dtb` 转成虚拟地址，校验 FDT 后读取 `memory` 节点的 `reg` 属性，得到物理内存基址和大小，供 PMM 使用。`page_init` 设置 `va_pa_offset` 和 `npage`，把 `Page` 描述符数组放在内核 `end` 之后并覆盖所有可管理物理页；它把已用区域标记为保留，只把剩余空闲页交给 `pmm_manager`。`pages` 是描述符数组的虚拟地址，`page2pa` 把描述符转换为对应物理页的起始地址，`pa2page` 把物理地址转换为对应描述符；其中需要通过物理页号计算数组下标。`struct Page` 登记的是物理页，描述符本身也占用内存，不是被管理的物理页内容。

### 2.3 物理页管理器的组织与分配算法

`pmm.h` 用 `struct pmm_manager` 把物理内存管理抽象为 `init`、`init_memmap`、`alloc_pages`、`free_pages`、`nr_free_pages`、`check` 六个接口，`Page` 用 `flags` 和 `property` 记录保留状态与空闲块大小。`default_pmm.c` 实现 First-Fit：按地址维护空闲链表，分配时取第一个足够大的块，释放时与相邻块合并。`best_fit_pmm.c` 沿用同样的结构，但分配时选择不小于请求页数且最小的块；完成注册后在 `init_pmm_manager` 中切换为 `best_fit_pmm_manager`。`pmm_init` 最后的 `check_alloc_page` 会调用管理器的 `check` 函数，通过分配和释放的自检确认实现可用。


### 2.4 伙伴系统的组织与运行

`buddy_pmm.c` 实现伙伴系统，按2的幂管理连续物理页，用 `order_lists` 保存各阶空闲块，`free_count` 记录空闲页数。`buddy_init()` 初始化链表与统计，`buddy_init_memmap()` 将可用区域划分为大小和起点对齐的块并登记。`buddy_alloc_pages()` 将请求向上取到2的幂，不足时拆分更大的块，例如申请3页实际占用4页。`buddy_free_pages()` 释放原分配块，`merge_block()` 只合并同阶、空闲且互为伙伴的块，并逐级尝试合并。通过 `buddy_pmm_manager` 接入统一管理接口，定义 `PMM_BUDDY` 后即可切换使用。它便于拆分和合并，但向上取整会产生内部碎片。

### 2.5 First-Fit与Best-Fit的改进空间

First-Fit沿链表找到第一个足够大的块就停止，开销随空闲块数量增加。可以增加按大小分类的索引、减少无效遍历，或使用Next-Fit从上次位置继续搜索；但这些方法会改变选择行为，需要比较搜索成本与碎片情况。地址有序链表便于释放时合并相邻块，应保留这一能力。

当前Best-Fit遍历全部空闲块来选最小合适块，容易留下很小的剩余块。可以用按大小组织的平衡树或分级链表加快查找，再保留地址索引用于合并；也可以研究小余块的处理策略。这些改进会增加元数据和维护成本，Best-Fit并不保证所有负载下碎片最少。

### 2.6 伙伴系统测试与覆盖理由

[buddy_check.c](kern/mm/buddy_check.c)通过管理器的 `check` 接口运行。测试建立受控区域，避免其他空闲块掩盖本应失败的分配，再检查以下行为：

| 测试内容 | 能检验的问题 |
|---|---|
| 耗尽后申请失败 | 不能超额分配，失败不能破坏统计 |
| 大块拆分、块起点对齐、递归合并 | 阶数、拆分位置与伙伴合并是否正确 |
| 相邻非伙伴块、3页请求占用4页 | 是否错误合并，以及向上取整与释放计数是否一致 |
| 写入并核对不同分配块的数据 | 分配区域是否重叠或被错误复用 |
| 非对齐起点的5页初始区间 | 是否遗漏页面、重复登记或越界 |
| 全部释放与内部不变量检查 | 空闲总量是否恢复，链表、范围与块状态是否一致 |

正常运行输出五组PASS并恢复31857个空闲页。零页申请、错误长度、内部页释放和重复释放属于会终止内核的负面用例，须分别运行，不能把正常自检的PASS当作这些分支已经执行。测试覆盖了主要行为和选定边界，不代表对所有内存配置的形式化证明。

## 3. 各练习的SPEC与提示词

### 3.1 提示词编写过程

我们先借助AI学习相关知识，弄清函数职责、数据结构和已有接口，再自己列出功能需求与需要处理的情况，形成提示词初稿。随后让AI补充遗漏的约束和边界场景，由小组成员对照指导书与源码审核，最后让AI调整表达。SPEC直接体现在提示词的功能、场景以及成功和失败结果中。审核后的提示词再用于实现与验证，发现问题后继续修订。

### 3.2 任务一：物理内存探测与页描述符初始化

胡博延（2513387）提交的初版提示词：

```text
在kern\mm\pmm.c 中实现 static void page_init(void)，并在文件顶部的 static 函数原型区补充它的前向声明。功能是：根据设备树提供的物理内存信息建立可管理物理页的描述符布局，并把除了 OpenSBI、内核镜像和描述符数组占用空间后的空闲内存交给当前 pmm_manager 管理。实现过程中可以使用get_memory_base()、get_memory_size()、panic()、cprintf()、init_memmap()、pa2page()、链接符号end、struct Page及相关全局变量，并使用框架提供的内存上界、地址转换、页面对齐和页面状态宏完成初始化。只修改 pmm.c，不改变 pmm_init 的调用顺序，也不改动其他文件。可以添加注释，但不要对原有的注释进行增添或修改。请直接修改真实文件并保存。

这个函数需要处理以下场景：
- 物理内存探测：用 get_memory_base() / get_memory_size() 得到物理内存基址和大小，并设置 va_pa_offset
- 描述符布局：设置npage和pages，数组长度覆盖物理页号 [nbase, npage]
- 已用空间排除：不把 OpenSBI 区、内核镜像（含 end 所在页）和 Page 描述符数组自身占用的页面交给物理内存管理器；
- 空闲页接管：通过 init_memmap把到 mem_end 为止的完整页面交给 pmm_manager。

函数成功时，应该保证：
- va_pa_offset 正确反映虚拟地址与物理地址之间的转换关系；
- npage正确表示可管理物理内存的页数上界；
- pages 指向的数组覆盖全部可管理物理页的描述符；
```

### 3.3 任务二：Best-Fit连续物理页分配

胡博延（2513387）提交的提示词：

```text
在 kern/mm/best_fit_pmm.c 中标有 lab2 function2-2 / YOUR CODE 的位置实现完整的 Best-Fit 物理内存管理模块，并在 kern/mm/pmm.c 的 init_pmm_manager() 中切换为 best_fit_pmm_manager。该文件只提供 basic_check() 和 best_fit_check() 两个测试函数；请阅读 pmm.h 中的 struct pmm_manager，按其成员职责自行设计内部状态和管理函数，并定义 const struct pmm_manager best_fit_pmm_manager 完成注册。可参考 default_pmm.c 的 First-Fit 实现，但分配策略必须改为 Best-Fit。只修改 best_fit_pmm.c 和 pmm.c 中 init_pmm_manager() 的赋值语句，不得修改 basic_check()、best_fit_check()、pmm.h、default_pmm.c 及其他文件；pmm.c 已包含 best_fit_pmm.h，无需新增 include。实现时可以使用 struct Page、free_area_t、list_* 接口、页面状态宏、PGSIZE、page2pa、page_ref / set_page_ref、alloc_pages / free_pages 及现有全局变量；管理函数签名与 pmm_manager 成员一致，辅助函数用 static。可以添加注释，但不要对原有的注释进行增添或修改。请直接修改真实文件并保存。

这个模块需要处理以下场景：
- 内部状态：在测试函数之前定义 free_area_t free_area 以及 free_list、nr_free，保证 best_fit_check() 能直接使用；
- 管理器注册：实现 init、init_memmap、alloc_pages、free_pages、nr_free_pages、check 六个函数，定义 best_fit_pmm_manager，check 指向 best_fit_check()；
- 空闲块登记：init_memmap 把 [base, base+n) 初始化为空闲，块首 property 设为 n 并按地址顺序插入 free_list，更新 nr_free；
- Best-Fit 分配：遍历所有空闲块，选择 property >= n 且 property 最小的块，剩余部分按地址顺序插回 free_list，更新 nr_free 并清除分配块首页的 PageProperty；
- 释放与合并：把 [base, base+n) 按地址顺序插入 free_list，与前后相邻的空闲块合并，更新 property 和 nr_free；
- 管理器切换：在 init_pmm_manager() 中把赋值改为 pmm_manager = &best_fit_pmm_manager，保留原有名称打印。

模块完成后，应该保证：
- pmm_manager 指向 best_fit_pmm_manager，启动输出 memory management: best_fit_pmm_manager；
- free_list 按地址升序排列，每个空闲块只有首页 PageProperty 为 1；
- 所有空闲块的 property 之和等于 nr_free_pages()，已分配页不在 free_list 中且首页 PageProperty 为 0；
- 空闲块大小为 1 页和 2 页时，申请 1 页返回那个 1 页块；
- 没有 property >= n 的空闲块时，alloc_pages(n) 返回 NULL；
- 连续释放的相邻页面会合并成单个空闲块，使随后的 alloc_pages(5) 成功；
- basic_check() 与 best_fit_check() 全部断言通过，make grade 输出 Test 1 和 Test 2 均为 PASS。
```

### 3.4 任务三：伙伴系统实现

李树青（2512885）整理的实现提示词：

```text
完成伙伴系统物理页分配模块，建议独立放在kern/mm/buddy_pmm.c和buddy_pmm.h，
功能是按2的幂次页数组织空闲块，通过拆分分配内存，通过伙伴合并回收大块，
并接入现有pmm_manager接口。测试由下一份提示词单独生成。

这个模块需要处理以下场景：
- 初始区域组织：覆盖上游提供的全部完整空闲页，支持起点未按大块对齐、长度不是2的幂的区域。
- 分阶管理：明确最大阶，以物理页号判断块的对齐和伙伴关系。
- 取整申请：请求n页时，实际占用向上取整为2的幂次页数；需要时拆分大块。
- 页面释放：接收原分配头和原始请求页数，归还整个实际块，不支持部分释放。
- 伙伴合并：仅同阶且属于同一对伙伴的空闲块可以递归合并，不能仅凭相邻合并。
- 内核接入：定义buddy_pmm_manager，提供必要声明及管理器选择分支，默认仍保留Best-Fit。

模块运行成功时：
- 提供与pmm.h一致的初始化、初始区域登记、分配、释放、空闲统计接口，并约定独立check入口。
- 分配块满足大小和对齐要求，不与其他块重叠，不包含保留页。
- 空闲页数按实际占用块大小减少或恢复，不按原始请求长度错误计数。
- 元数据能支持原始请求与实际块大小核对；释放后可合并的伙伴正确合并。
- 各阶链表、块状态和统计一致，初始区域没有遗漏、重复或越界登记。
- 通过已有DEFS=-DPMM_BUDDY编译参数可单独运行；不修改Makefile，切换前要求make clean。

分配失败或遇到特殊情况时：
- 请求超过支持大小、空闲不足或无法形成所需块时，返回NULL且状态不变。
- n==0时断言失败；错误块头、重复释放或长度不匹配时明确拒绝，不能静默破坏管理状态。
- 缺少独立测试或未实际运行时，不宣称Buddy已经验证正确。

只新增伙伴系统模块和必要接入，不覆盖Best-Fit、First-Fit或它们的原有测试，不添加SLUB。
说明最大阶、内部碎片及其他限制，不要求复杂度优化。
```

### 3.5 任务三：伙伴系统测试

李树青（2512885）整理的测试提示词：

```text
针对实际生成的伙伴系统模块，独立设计测试代码，建议放在kern/mm/buddy_check.c，
功能是检验初始化、分配、拆分、释放、合并和错误处理是否符合上一份规格，
并接入buddy_pmm_manager的check入口。

这些测试需要处理以下场景：
- 基本分配：单页、不同阶块及非2次幂请求，核对大小、对齐和实际占用。
- 拆分与合并：多次拆分、逐级合并，以及相邻但不是伙伴的块不得合并。
- 初始区域边界：非大块对齐起点和非2次幂长度，检查无遗漏、重复或越界。
- 资源不足：耗尽、超大请求，以及总量够但无法形成所需块。
- 数据与恢复：交错申请释放，写入不同数据核对无重叠，全部归还后检查状态恢复。
- 非法调用：零页请求、错误起点、错误长度及重复释放。

测试成功时：
- 每项有明确准备条件、操作、预期结果及实际结果，预期来自规格而非照抄实现。
- 受控场景中的失败或合并行为不被其他空闲块掩盖。
- 分配大小、内容、状态及页数统计符合约定，归还后恢复原空闲总量及内部不变量。
- 初始区域测试在隔离池中进行，不覆盖内核、描述符或仍在使用的分配；结束后恢复状态。
- 交付独立测试代码、运行命令、覆盖说明，并解释这些测试能发现什么错误。

测试失败或遇到特殊情况时：
- 不符合规格时报告最小失败例，不改预期或修改算法来掩盖错误。
- 预期panic的非法调用单独运行，记录是否在预期位置拒绝，不能中断主测试后声称全部通过。
- 无法构造、未运行或环境不支持的用例明确标注，不用静态分析冒充运行结果。

只增加测试及必要check接入，不修改既定算法规格或First-Fit、Best-Fit原有测试。
```

## 4. AI生成代码的问题与处理

任务一、二初版默认检查20/20，但交叉验证发现：内存增大时未限制框架上界，没有完整空闲页时仍调用地址转换。胡博延修订后补充了上界和区间判断，但仅剩1字节时仍报错，内存大小为0时也未主动拒绝。

初版提示词没有明确这些边界结果。李树青（2512885）进一步补充范围有效性、溢出和描述符容量检查，统一页对齐，并仅在完整空闲页数大于0时转换和登记；同时完成伙伴系统融合。

修订后老师检查20/20，Best-Fit与伙伴系统正常检查通过，256 MiB上界、无完整空闲页、仅剩1字节、零大小、错误基址、溢出及描述符空间不足用例均符合预期。错误分支在临时副本中验证，原测试未修改。

## 5. 实验重要知识点与OS原理的对应

| 知识点 | 实验中的含义与对应 | 与OS原理的联系及差异 |
|---|---|---|
| 虚拟地址与物理地址 | 内核链接地址与加载地址不同，启动页表完成映射 | 原理中用映射实现地址空间抽象；本实验采用固定偏移的内核直接映射，该公式不适用于任意虚拟地址 |
| 分页与页描述符 | 物理内存按4 KiB划页，`Page`保存页状态，PTE保存映射与权限 | `Page`是OS管理物理页的数据结构，PTE是硬件翻译地址所读的条目，二者职责不同 |
| 多级页表与大页 | Sv39支持三级索引；启动根页表直接使用1 GiB叶节点 | 多级页表节省稀疏映射的页表空间，大页减少遍历层数；本次启动翻译没有真正走完三级 |
| 连续内存分配与碎片 | First-Fit、Best-Fit选择空闲块；Buddy按阶拆分与合并 | 对应分配策略与内外部碎片的权衡；本实验分配连续物理页，没有实现任意字节对象分配 |
| 资源发现与保留 | DTB提供内存范围，初始化排除OpenSBI、内核与描述符占用 | 对应启动时建立资源管理视图；固件报告的内存范围不等于可立即分配的空闲范围 |
| TLB与局部性 | GDB观察页表遍历、TLB填充和QEMU快路径 | 原理中的TLB缓存地址翻译；QEMU用软件结构与生成代码模拟，不能据此推断真实硬件时延 |
| 机制与策略分离 | `pmm_manager`统一接口，选择不同分配器 | 接口负责提供分配能力，具体算法决定选哪块；切换管理器不需要重写所有调用者 |

## 6. OS原理中重要但本实验未实现的知识点

- **按需分页与页面置换**：完整系统可在缺页时装入页面，并在内存不足时换出；本实验重点是物理页初始化与分配，没有实现换入换出。
- **进程独立地址空间**：原理中各进程拥有页表，切换时需要处理页表与TLB；本实验使用启动内核映射，没有实现完整的多进程地址空间切换。
- **用户态隔离与写时复制**：页权限可隔离用户和内核，也可支持共享页面后按写操作复制；本实验未建立相应进程与缺页处理机制。
- **并发内存管理**：多核同时申请和释放需要锁、原子操作及跨核TLB同步；本次验证没有覆盖多核并发分配和一致性协议。
- **任意大小对象分配**：页级分配不能直接解决大量小对象的高效管理；SLUB属于另列的Challenge，本组本次未实现。

## 7. 分支任务：QEMU/GDB页表调试

负责人：杨永琛（2513097）。以下基于其提交报告合并，统一章节编号，保留原有命令与观测数据。校对时将示例地址的VPN1更正为1，并收紧“断点未命中即可证明TLB命中”的结论；

### 7.1. 实验目标

本实验使用三个终端和两个 GDB 会话，同时观察：

1. QEMU 模拟器内部的地址转换实现；
2. ucore 客户机中的 RISC-V 指令、寄存器、页表和 `satp`。

重点验证：

- TLB miss 后如何进入 RISC-V 页表遍历；
- Sv39 根页表项如何转换为客户机物理地址；
- 页表遍历结果如何写入 QEMU 软件 TLB；
- TLB 命中后为什么不再次遍历页表；
- 分页关闭时 TLB、地址转换和页表遍历有何区别。

### 7.2. 调试环境

| 项目 | 路径或版本 |
|---|---|
| ucore 工程 | `/mnt/c/Users/YANGYONGCHEN/Desktop/lab2_new` |
| QEMU 源码 | `/home/yangyongchen/qemu/qemu-4.1.1` |
| 调试版 QEMU | `/home/yangyongchen/qemu/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64` |
| 宿主机 GDB | `/usr/bin/gdb`，GDB 17.1，目标架构 x86-64 |
| RISC-V GDB | `riscv64-unknown-elf-gdb`，GDB 10.1，目标架构 `riscv64-unknown-elf` |

调试版 QEMU 经 `file` 检查带有调试信息：

```text
ELF 64-bit LSB pie executable, x86-64
with debug_info, not stripped
```

### 7.3. 两个 GDB 的职责

#### 7.3.1 宿主机 GDB

宿主机 GDB 是 x86-64 原生 GDB，用于附加到 QEMU 的 Linux 进程，调试 QEMU 自身的 C 源码。

典型用途：

- 在 `riscv_cpu_tlb_fill()`、`get_physical_address()`、`tlb_set_page()` 等函数中设置断点；
- 查看 QEMU 内部参数、局部变量和调用栈；
- 观察 QEMU 如何用软件模拟 RISC-V MMU 和 TLB。

#### 7.3.2 RISC-V GDB

RISC-V GDB 是交叉调试器，通过 QEMU 的 GDB stub 连接客户机 CPU。

典型用途：

- 加载 `bin/kernel` 的 ucore 符号；
- 控制客户机执行；
- 查看客户机 `pc`、`satp` 和 RISC-V 指令；
- 按客户机虚拟地址查看内存。

两个 GDB 都运行在 x86-64 主机上，但目标架构不同：宿主机 GDB 调试 QEMU 进程，RISC-V GDB 调试被模拟的 RISC-V 客户机。

### 7.4. 启动流程

终端 1 启动调试版 QEMU：

```bash
cd /mnt/c/Users/YANGYONGCHEN/Desktop/lab2_new

make debug QEMU=/home/yangyongchen/qemu/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64
```

Makefile 中的 `debug` 目标使用：

```text
-s 开放 QEMU GDB stub，监听 1234 端口
-S 启动后暂停客户机 CPU
```

终端 2 查询 QEMU PID：

```bash
pgrep -af '/home/yangyongchen/qemu/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64'
```

其中包含 `make debug` 的是父进程，包含 `-s -S` 的才是真正的 QEMU 进程。

随后附加宿主机 GDB：

```bash
sudo gdb /home/yangyongchen/qemu/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64
```

```text
set pagination off
set confirm off
attach <实际 PID>
handle SIGPIPE nostop noprint
```

宿主机GDB附加后会暂停QEMU进程，需先设置好断点并执行 `continue`，让QEMU能够响应客户机GDB连接；客户机仍受 `-S` 控制等待。后续宿主机命中断点时，也需要在相应会话继续执行。

终端 3 启动客户机 GDB：

```bash
export PATH="/mnt/d/WSL/OS/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:$PATH"

cd /mnt/c/Users/YANGYONGCHEN/Desktop/lab2_new
make gdb
```

连接后客户机 CPU 初始位于：

```text
pc   = 0x1000
satp = 0
```

执行 RISC-V GDB 的 `continue` 后，OpenSBI 和 ucore 才开始运行。

### 7.5. ucore 启动时观察到的重要数据

两次运行中，OpenSBI 和 ucore 的启动信息一致：

```text
OpenSBI Firmware Base: 0x80000000
DTB Address:           0x82200000
Physical Memory:       0x80000000 - 0x87ffffff
Memory Size:           128 MB
Kernel entry:          0xffffffffc02000d8
Kernel page table VA:  0xffffffffc0205000
Kernel page table PA:  0x0000000080205000
Memory manager:        best_fit_pmm_manager
```

其中启动页表是一个覆盖 1 GiB 的根页表，根页表物理基址为：

```text
0x80205000
```

### 7.6. 第一次运行：开启 Sv39 后的地址转换

#### 7.6.1 条件断点

宿主机 GDB 在 `kern_init` 的高虚拟地址处翻译取指时暂停：

```text
break riscv_cpu_tlb_fill if address == 0xffffffffc02000d8
continue
```

观察结果：

```text
address     = 0xffffffffc02000d8
access_type = MMU_INST_FETCH
mmu_idx     = 1
```

这表明本次转换服务于 S 模式下的指令取指。

#### 7.6.2 调用路径

实际 GDB 调用栈为：

```text
code_gen_buffer
  -> helper_lookup_tb_ptr
    -> tb_htable_lookup
      -> get_page_addr_code
        -> tlb_fill
          -> riscv_cpu_tlb_fill
```

`tb_htable_lookup()` 查询的是 QEMU 的指令翻译块缓存，不是 TLB。由于控制流跳转到新的高虚拟地址，QEMU 需要先通过 `get_page_addr_code()` 确认该页的指令 TLB 映射。

#### 7.6.3 进入页表遍历

继续设置：

```text
break get_physical_address if addr == 0xffffffffc02000d8
continue
```

观察结果：

```text
addr      = 0xffffffffc02000d8
satp      = 0x8000000000080205
priv      = 1
levels    = 3
ptidxbits = 9
base      = 0x80205000
```

`satp` 拆解：

```text
MODE = 8，表示 Sv39
PPN  = 0x80205
根页表物理地址 = 0x80205 << 12 = 0x80205000
```

#### 7.6.4 页表项读取

在 QEMU 源码 `target/riscv/cpu_helper.c` 中：

```c
for (i = 0; i < levels; i++, ptshift -= ptidxbits) {
    idx = (addr >> (PGSHIFT + ptshift)) &
          ((1 << ptidxbits) - 1);
    pte_addr = base + idx * ptesize;
    pte = ldq_phys(cs->as, pte_addr);
    ppn = pte >> PTE_PPN_SHIFT;
}
```

这是一个最多执行三次的页表遍历循环：

| 迭代 | `ptshift` | 索引 | 页表层级 |
|---|---:|---|---|
| `i=0` | 18 | VPN2 | 根页表 |
| `i=1` | 9 | VPN1 | 第二级页表 |
| `i=2` | 0 | VPN0 | 第三级页表 |

本次实际只执行了 `i=0`：

```text
i        = 0
idx      = 0x1ff
pte_addr = 0x80205ff8
pte      = 0x200000cf
ppn      = 0x80000
```

PTE 标志验证：

```text
pte & PTE_V   = 0x1
pte & (R|W|X) = 0xe
```

因此该 PTE 是叶节点，并且表示 1 GiB 超级页。它不需要继续查询第二级和第三级页表。

#### 7.6.5 物理地址计算

虚拟地址拆分：

```text
VPN2   = 0x1ff
VPN1   = 0x001
VPN0   = 0x000
offset = 0xd8
```

PTE 中的物理页号：

```text
PPN = 0x80000
物理基址 = 0x80000 << 12 = 0x80000000
```

最终计算结果：

```text
vpn       = 0xffffffffc0200
ppn       = 0x80000
ptshift   = 18
*physical = 0x80200000
```

这里 `0xd8` 是4 KiB页内偏移；根级1 GiB叶节点使用低30位作为大页内偏移，即 `0x2000d8`。因此也可直接算 `0x80000000 + 0x2000d8 = 0x802000d8`。上面的 `*physical` 是本次翻译得到的4 KiB页起始地址，加上 `0xd8` 后目标访问地址是：

```text
0x80200000 + 0xd8 = 0x802000d8
```

#### 7.6.6 写入软件 TLB

页表遍历成功后，QEMU 调用：

```text
tlb_set_page(vaddr, paddr, prot, mmu_idx, size)
```

实际观察结果：

```text
vaddr = 0xffffffffc0200000
paddr = 0x80200000
prot  = 7
size  = 4096
```

`prot=7` 表示：

```text
读 | 写 | 执行
```

TLB 项的地址标签和宿主偏移为：

```text
tn.addr_code  = 0xffffffffc0200000
tn.addr_read  = 0xffffffffc0200000
tn.addr_write = 0xffffffffc0200400
tn.addend     = 0x7ec8b3e00000
```

`addr_write` 中多出的 `0x400` 是 QEMU 内部标志：

```c
TLB_NOTDIRTY
```

去掉标志后仍然是：

```text
0xffffffffc0200400 & ~0x400
= 0xffffffffc0200000
```

`tn.addend` 是 QEMU 宿主地址空间中的偏移，不是客户机物理地址。TLB 命中时，QEMU 可通过：

```text
host_addr = guest_vaddr + tn.addend
```

快速定位客户机 RAM 在 QEMU 进程中的宿主地址。

#### 7.6.7 TLB 命中验证

TLB 安装完成后，清除旧断点，仅保留：

```text
break tlb_fill if addr == 0xffffffffc0200000
continue
```

随后 QEMU 继续运行，但该断点没有再次命中。ucore 最终运行到：

```text
kern_init () at kern/init/init.c:43
43     while (1)
```

RISC-V GDB 显示：

```text
pc   = 0xffffffffc0200110
satp = 0x8000000000080205

0xffffffffc0200110 <kern_init+56>: j 0xffffffffc0200110
```

由此可得结论：

> 本次明确观察到了首次翻译的页表遍历与TLB安装，以及继续运行后该条件断点未再次命中。不过，条件仅匹配页起始地址，可能漏掉同页其他偏移；已翻译的TB也可能被直接复用。因此这一现象与缓存复用相符，不能单独证明后续每次取指都走TLB命中快路径。若继续验证，应按页掩码设置条件，并结合快路径与TB执行位置观察。

### 7.7. 第二次运行：关闭分页时的地址转换

第二次运行在分页尚未开启时观察内核物理入口：

```text
break riscv_cpu_tlb_fill if address == 0x80200000
```

实际观察结果：

```text
address     = 0x80200000
access_type = MMU_INST_FETCH
mmu_idx     = 1
```

调用路径为：

```text
cpu_exec
  -> tb_find
    -> tb_htable_lookup
      -> get_page_addr_code
        -> tlb_fill
          -> riscv_cpu_tlb_fill
            -> get_physical_address
```

随后执行继续到 QEMU 源码中的 Bare 模式返回位置：

```text
cpu_helper.c:199
return TRANSLATE_SUCCESS;
```

实际读取：

```text
addr      = 0x80200000
*physical = 0x80200000
```

这次命中的是 `VM_1_10_MBARE` 分支。虽然 QEMU 仍然查询了软件 TLB，并在未命中时调用了 `riscv_cpu_tlb_fill()` 和 `get_physical_address()`，但没有进行 Sv39 页表遍历，而是直接令：

```text
physical = virtual
```

这说明“TLB miss”与“页表遍历”不是同一件事：

```text
TLB miss
    -> 调用地址转换函数
        -> 可能是 Bare 模式直接映射
        -> 也可能是 Sv39 页表遍历
```

### 7.8. QEMU 中的 TLB 快路径

QEMU 通常不会在每次访存时调用一个名为 `tlb_lookup()` 的 C 函数。TCG 会生成宿主 x86 指令，在动态代码中完成 TLB 快路径。

关键代码位置：

```text
accel/tcg/cputlb.c
    get_page_addr_code()       第 1023 行附近
    tlb_fill()                 第 868 行附近
    tlb_set_page_with_attrs()  第 700 行附近
    store_helper()             第 1500 行附近

tcg/i386/tcg-target.inc.c
    tcg_out_tlb_load()         第 1699 行附近

target/riscv/cpu_helper.c
    get_physical_address()     第 155 行附近
    riscv_cpu_tlb_fill()       第 435 行附近
```

x86 TCG 后端生成快路径时，核心逻辑是：

1. 根据客户机虚拟地址计算 TLB 索引；
2. 比较 TLB 中缓存的虚拟页标签；
3. 标签相同则命中，将客户机虚拟地址加上 `addend` 得到宿主地址；
4. 标签不同则跳到慢路径，最终调用 `helper_le_ld/st*_mmu()`。

`tlb_fill()` 本身不遍历页表。它调用 CPU 类中的架构相关回调，RISC-V 对应的是：

```text
tlb_fill()
  -> cc->tlb_fill()
    -> riscv_cpu_tlb_fill()
      -> get_physical_address()
```

### 7.9. QEMU 软件 TLB 与真实 CPU TLB 的区别

| 真实 CPU TLB | QEMU TCG TLB |
|---|---|
| 硬件 MMU 内部的地址转换缓存 | QEMU 进程中的软件数据结构 |
| 缓存虚拟页、物理页和权限 | 缓存虚拟页、权限及宿主地址 `addend` |
| 命中由硬件透明完成 | 由 TCG 动态生成的宿主代码完成 |
| 关注组关联、容量和替换策略 | 主要保证功能正确，不精确模拟时序 |
| 失效由 MMU 和架构指令控制 | 由 QEMU 软件执行 TLB flush |
| 通常只保存客户机 VA 到 PA 的转换 | 还保存 `NOTDIRTY`、`MMIO`、`RECHECK` 等模拟器标志 |

QEMU 还实现了 victim TLB。主 TLB 未命中时会先尝试 victim TLB，只有仍未命中才进入完整慢路径。

### 7.10. 调试中遇到的问题及处理

#### 7.10.1 `<PID>` 是占位符

错误：

```text
attach <PID>
Illegal process-id: <PID>.
```

正确处理：

```text
attach 15646
```

必须用 `pgrep -af` 查到的实际数字替换 `<PID>`。

#### 7.10.2 QEMU 启动后不返回 shell

`make debug` 启动的 QEMU 前台运行，并因 `-s -S` 一直等待 GDB，所以终端不会回到 shell。这是正常现象，应保持终端 1 不动，另开终端 2 和终端 3。

#### 7.10.3 `SIGPIPE`

宿主机 GDB 附加 QEMU 后需要设置：

```text
handle SIGPIPE nostop noprint
```

否则客户机 GDB 断开、重连或 QEMU GDB stub 关闭 socket 时，宿主机 GDB 可能因 `SIGPIPE` 突然停下来，导致 QEMU 看起来卡住。

#### 7.10.4 GDB 分页提示

RISC-V GDB 初次连接时可能出现：

```text
--Type <RET> for more, q to quit, c to continue without paging--
```

这里输入 `c` 只是让 GDB 继续显示输出，不是 `continue`。之后可执行：

```text
set pagination off
```

#### 7.10.5 `mode` 显示 `<optimized out>`

QEMU 启用了优化，部分局部变量会被优化掉。`mode` 初始值为：

```c
int mode = mmu_idx;
```

本次 MPRV 条件不成立，所以可以使用：

```text
mmu_idx = 1
```

得到临时结论 `mode = 1`，不影响页表遍历实验。

#### 7.10.6 条件断点读取被优化变量

错误：

```text
Error in testing condition for breakpoint:
value has been optimized out
```

例如条件中使用了 `vaddr_page`。可将条件改为仍然存在的函数参数：

```text
condition <断点编号> vaddr == 0xffffffffc0200000
```

或者利用当前已经位于目标调用中，直接设置无条件的行断点：

```text
break cputlb.c:835
continue
```

#### 7.10.7 “Attempt to use a type name as an expression”

如果在 `tlb_n_used_entries_inc()` 第 0 帧直接打印 `vaddr`，会出现该错误，因为当前函数没有这个局部变量，GDB 将 `vaddr` 当成类型名。

解决：

```text
bt
frame 1
info args
p/x vaddr
```

正确的帧是：

```text
#0 tlb_n_used_entries_inc()
#1 tlb_set_page_with_attrs()
```

#### 7.10.8 重复断点

对同一行重复设置条件断点时，GDB 会提示已有断点并创建重复断点。可使用：

```text
info breakpoints
delete <重复编号>
```

避免 QEMU 在同一位置暂停多次。

#### 7.10.9 分页开启后无法用低物理地址读取 PTE

RISC-V GDB 的内存访问按客户机当前虚拟地址空间处理。分页开启后可以读取：

```text
x/gx 0xffffffffc0205ff8
```

该虚拟地址经 1 GiB 映射转换为物理地址 `0x80205ff8`，结果为：

```text
0x00000000200000cf
```

直接读取低物理地址 `0x80205ff8` 会失败。需要从物理地址查看时，可通过 QEMU monitor：

```text
monitor xp /gx 0x80205ff8
```

#### 7.10.10 glibc 源码警告

附加 QEMU 后停在 `__syscall_cancel_arch()` 并提示 glibc 源文件不存在，这是因为只安装了运行库而非 glibc 调试源码，不影响 QEMU 调试。

### 7.11. 实验结论

1. ucore 启动页表通过 `satp=0x8000000000080205` 启用 Sv39，根页表物理地址为 `0x80205000`。
2. 虚拟地址 `0xffffffffc02000d8` 的 VPN2 是 `0x1ff`，查询根页表项 `0x80205ff8`。
3. PTE 为 `0x200000cf`，有效且包含读写执行权限，是映射 1 GiB 的叶节点，因此只执行一次页表循环。
4. 本次页表遍历得到的物理页为 `0x80200000`，加上页内偏移 `0xd8` 后得到 `0x802000d8`。
5. `tlb_set_page()` 将客户机虚拟页 `0xffffffffc0200000` 映射到物理页 `0x80200000`，并保存权限和宿主地址偏移。
6. TLB命中可以避免重复页表遍历；本次断点未再次命中的现象还受到条件范围与TB复用影响，不能独立证明每次后续取指都命中TLB。
7. 分页关闭时仍可能发生 TLB miss 并调用地址转换函数，但 Bare 模式直接令 `physical = virtual`。
8. `Translation Block` 缓存和 TLB 是两个不同层次：前者缓存翻译后的指令块，后者缓存地址转换结果。

### 7.12. 原始运行记录

- `C:\Users\YANGYONGCHEN\Desktop\第一次运行结果.txt`
- `C:\Users\YANGYONGCHEN\Desktop\第二次运行结果.txt`

据同学报告，第一次运行记录覆盖Sv39页表查询、TLB安装与后续运行观察，第二次覆盖 `satp=0` 时的Bare模式直接映射。以上两个txt为同学电脑上的原始记录路径，本次仅收到总结Markdown，未将未提供的txt列为已随报告归档的附件。
