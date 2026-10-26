# 实验二完整代码：任务一、任务二与伙伴系统融合版

## 代码来源

- 物理内存初始化、Best-Fit：胡博延，2513387。
- page_init边界协助修订及工程融合：李树青，2512885。
- 伙伴系统实现与独立测试：李树青，2512885。
- 分支调试负责人：杨永琛，2513097；本包不包含尚未提交的调试结果。

源码中的lab2标记已注明对应姓名和学号，原框架测试未修改。

## 包内内容

这是完整的lab2_new工程，包含Makefile、kern、libs、tools及老师测试工具。
不包含旧的obj/bin、过程备份和未完成报告。解压后应在Makefile所在目录构建。
需要WSL/Linux、make、RISC-V交叉工具链及QEMU；将工具放入PATH。
本组已验证环境为riscv64-unknown-elf工具链10.2.0和QEMU 4.1.1。

## 默认运行Best-Fit

```bash
cd lab2_new
make clean
make qemu
```

输出应包含：

```text
memory management: best_fit_pmm_manager
check_alloc_page() succeeded!
```

内核随后空转正常。退出QEMU按Ctrl+A，松开后按X。
老师检查使用make grade，当前已验证20/20。

## 切换伙伴系统

```bash
make clean
make DEFS=-DPMM_BUDDY qemu
```

输出应包含memory management: buddy_pmm_manager、五组[Buddy] PASS和最终检查成功。
每次切换编译参数都必须先make clean；两个算法不是同时管理内存。
申请3页实际占用4页，释放时传原始块头和原始请求3；不支持部分释放。

## 切回Best-Fit

先退出QEMU，再执行：

```bash
make clean
make qemu
```

## 修改的代码在哪里

以下路径相对于解压后的lab2_new目录，行号对应本包版本；后续编辑后可能变化，可按函数名或lab2注释搜索。

| 任务/文件 | 定位位置 | 具体实现或修改 | 署名 |
|---|---|---|---|
| [kern/mm/pmm.c](kern/mm/pmm.c) | 第71～76行附近，`lab2 function1`、`page_init()` | 任务一：读取RAM信息，布置Page数组，保护已占用页，登记完整空闲页 | 胡博延2513387；边界协助修订李树青2512885 |
| [kern/mm/pmm.c](kern/mm/pmm.c) | 第38行，`init_pmm_manager()` | 融合接入：`#ifdef PMM_BUDDY`选择伙伴系统，否则默认Best-Fit | 李树青2512885 |
| [kern/mm/best_fit_pmm.c](kern/mm/best_fit_pmm.c) | 第6行`lab2 function2-2`；管理函数在第15～130行，注册在第266行 | 任务二完整Best-Fit实现；原有`basic_check()`、`best_fit_check()`保留 | 胡博延2513387 |
| **[kern/mm/buddy_pmm.c](kern/mm/buddy_pmm.c)** | **第4行`lab2 function2-3`** | **任务三算法实现：各阶链表、块拆分、伙伴合并、分配释放、统计与管理器注册** | **李树青2512885** |
| [kern/mm/buddy_pmm.h](kern/mm/buddy_pmm.h) | 文件中的`lab2 function2-3-interface` | 任务三接口、最大阶数及释放约定 | 李树青2512885 |
| [kern/mm/buddy_check.c](kern/mm/buddy_check.c) | 第4行`lab2 function2-3-test`；第36行`buddy_check()` | 任务三独立测试；第13行`negative_check()`提供隔离错误用例 | 李树青2512885 |

### page_init具体修了哪些问题

在`kern/mm/pmm.c`的`page_init()`中修订：

1. **读取RAM信息之后**：检查大小为0、起点与框架不兼容、地址相加溢出等错误，调用`panic()`拒绝无效范围。
2. **确定管理末端时**：取设备树终点和`PHYSICAL_MEMORY_END`的较小值，再用`ROUNDDOWN`向下按页对齐。
3. **写入Page数组之前**：检查管理范围是否容纳内核和整个描述符数组，防止先写入再发现越界。
4. **初始化每张登记卡时**：先清除旧字段，设置保留状态，并初始化引用计数、块长度和链表节点。
5. **交给分配器之前**：计算`free_count`，只有大于0才调用`pa2page()`和`init_memmap()`；无剩余页或只剩不足一页时不转换、不登记。
6. **输出信息**：打印探测范围、实际管理上界、描述符数量、空闲起点和完整空闲页数。

### 伙伴系统主要函数

- `buddy_init()`，第42行：准备各阶空闲链表。
- `buddy_init_memmap()`，第47行：把初始区域组织为对齐的2次幂块。
- `buddy_alloc_pages()`，第66行：取整、找块和拆分。
- `buddy_free_pages()`，第92行：验证释放参数、归还和合并。
- `buddy_validate()`，第113行：检查空闲结构与统计的不变量。
- `buddy_pmm_manager`，第146行：注册算法及独立测试入口。

除上述任务实现、必要接入及署名外，未为本次融合修改Makefile、启动汇编、公共结构体或老师原有测试。

## 本轮修复与验证

page_init增加无效RAM、地址溢出、描述符容量检查，限制管理上界，
按页对齐并仅在存在完整空闲页时执行转换和登记，完整初始化描述符。
老师检查20/20，Best-Fit与伙伴系统正常启动自检通过；
256MiB上界、无完整空闲页、1字节尾部、零大小、错误基址、溢出及数组放不下用例已在临时副本验证。
这不表示所有可能情况均已穷尽。实现用于当前实验单核框架。

SHA256SUMS.txt记录包内文件校验值，便于确认分享时文件未损坏。
