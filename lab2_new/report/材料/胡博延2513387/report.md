## 三、实验整体逻辑分析

### 3.1 逻辑主线

本实验在 Lab1 基础上把内核从物理地址空间迁到虚拟地址空间，并建立物理内存页的管理机制。内核链接在高虚拟地址、加载在低物理地址；`entry.S` 用预置页表把最高 1 GiB 虚拟地址映射到物理内存起始处，打开 Sv39 后以虚拟地址运行。进入 `kern_init` 后，内核清零 BSS、从设备树获取内存范围，再由 `pmm_init` 建立 `Page` 描述符和空闲页链表，最后通过自检确认分配器可用。整体主线是：切换地址空间、获取内存信息、管理物理页、验证分配算法。

### 3.2 构建与启动的顺序

构建时 Makefile 完成编译、链接和镜像转换，`kernel.ld` 以 `0xFFFFFFFFC0200000` 为基址并指定 `kern_entry`；QEMU 把生成的 `ucore.img` 加载到 `0x80200000`，因此同一份代码同时具有虚拟地址和物理地址，二者相差固定偏移。

启动时 OpenSBI 把 hartid 和 DTB 地址交给内核并跳到 `0x80200000`；`kern_entry` 保存参数、建立 `satp`、刷新 TLB，然后把 `sp` 和 PC 切换到虚拟地址并进入 `kern_init`。`kern_init` 依次完成 BSS 清零、`dtb_init`、控制台输出和 `pmm_init`，最后进入循环。

## 四、核心函数与功能模块理解

### 4.1 启动页表与地址切换

`entry.S` 的 `boot_page_table_sv39` 只有一个有效页表项，把虚拟地址最高 1 GiB 映射到物理 `0x80000000` 起的 1 GiB 窗口，其中实际有内存的是前 128 MiB。打开分页前代码按物理地址执行，因此计算页表物理页号时要减去虚实偏移；写入 `satp` 后，`sp` 和 PC 都改用虚拟地址，此后内核符号地址都是链接地址。这张页表只完成内核启动所需的直接映射，不涉及动态建立映射。

### 4.2 设备树探测与页描述符布局

`dtb_init` 把 `boot_dtb` 转成虚拟地址，校验 FDT 后读取 `memory` 节点的 `reg` 属性，得到物理内存基址和大小，供 PMM 使用。`page_init` 设置 `va_pa_offset` 和 `npage`，把 `Page` 描述符数组放在内核 `end` 之后并覆盖所有可管理物理页；它把已用区域标记为保留，只把剩余空闲页交给 `pmm_manager`。`pages` 是描述符数组的虚拟地址，`page2pa` 和 `pa2page` 负责页号与数组下标之间的换算。

### 4.3 物理页管理器的组织与分配算法

`pmm.h` 用 `struct pmm_manager` 把物理内存管理抽象为 `init`、`init_memmap`、`alloc_pages`、`free_pages`、`nr_free_pages`、`check` 六个接口，`Page` 用 `flags` 和 `property` 记录保留状态与空闲块大小。`default_pmm.c` 实现 First-Fit：按地址维护空闲链表，分配时取第一个足够大的块，释放时与相邻块合并。`best_fit_pmm.c` 沿用同样的结构，但分配时选择不小于请求页数且最小的块；完成注册后在 `init_pmm_manager` 中切换为 `best_fit_pmm_manager`。`pmm_init` 最后的 `check_alloc_page` 会调用管理器的 `check` 函数，通过分配和释放的自检确认实现可用。
