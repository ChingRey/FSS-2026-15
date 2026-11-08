# 实验三融合工程

沿用实验二最终版的物理内存初始化、Best-Fit和Buddy，合入Lab3_code(1).zip中的实验三入口恢复及异常、中断处理代码，关键位置补充中文注释。实验三新增的原子页标志和分配/释放期间的中断保护保留。

## 运行

在WSL中进入：

```bash
cd /mnt/c/Users/十三月/Desktop/系统软件/lab3/lab3_new
make clean
make qemu
```

默认Best-Fit，正常应输出内存自检成功、十次100 ticks，随后关机。

切换伙伴系统：

```bash
make clean
make DEFS=-DPMM_BUDDY qemu
```

应额外出现Buddy的五组自检通过。每次切换参数必须先清理。

课程评分：`make grade`。在当前共享环境建议指定独立临时目录：`make grade GRADE_TMP=/tmp/lab3_merged_manual_grade`。

## 阅读顺序

1. kern/init/init.c：清零BSS、读取设备树、初始化内存与中断、开启中断。
2. kern/mm/pmm.c：page_init读取内存并建立页描述符；分配器默认Best-Fit，可选Buddy；alloc_pages/free_pages在临界区保存和恢复中断状态。
3. kern/trap/trapentry.S：SAVE_ALL保存现场，a0传现场地址，调用trap；RESTORE_ALL恢复，最后恢复sp，再sret返回。
4. kern/trap/trap.c：trap分发中断和异常；interrupt_handler维护持续时钟与关机；exception_handler跳过断点或非法指令。
5. kern/driver/clock.c：通过OpenSBI预约下次时钟事件。
6. kern/sync/sync.h：保存、关闭、恢复中断，保护内存管理链表。

## 来源与署名

实验二原署名保留：物理初始化和Best-Fit为胡博延2513387；边界修订、融合和Buddy为李树青2512885。按本次确认的署名安排：实验三RESTORE_ALL标注胡博延2513387，__alltraps入口调用标注李树青2512885，中断与异常处理标注杨永琛2513097。

手册依据：[练习](http://oslab.mobisys.top/lab2026/_book/lab3/lab3_2_1_exercise.html)、[入口](http://oslab.mobisys.top/lab2026/_book/lab3/lab3_3_2_trapentry.html)、[时钟](http://oslab.mobisys.top/lab2026/_book/lab3/lab3_3_4_clock.html)、[中断保护](http://oslab.mobisys.top/lab2026/_book/lab3/lab3_3_5_atomic.html)。

融合前工程保存在上级“过程记录/融合前备份_时间”目录。验证日志位于“过程记录/融合验证”。本说明不代替实验报告。

## 本次实测

融合后重新编译成功；Best-Fit启动自检通过，打印十次100 ticks后退出；课程评分100/100；Buddy五组自检全部通过，同样打印十次后正常退出。课程测试和评分脚本保持不变。未进行交互式GDB单步调试。日志位于上级过程记录/融合验证。
