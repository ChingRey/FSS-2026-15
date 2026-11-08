#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <sbi.h>
#include <stdio.h>
#include <trap.h>

#define TICK_NUM 100
#define SHUTDOWN_AFTER 10

static void print_ticks() {
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S
 */
void idt_init(void) {

    extern void __alltraps(void);
    /* Set sup0 scratch register to 0, indicating to exception vector
       that we are presently executing in the kernel */
    write_csr(sscratch, 0);
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps);
}

/* trap_in_kernel - test if trap happened in kernel */
bool trap_in_kernel(struct trapframe *tf) {
    return (tf->status & SSTATUS_SPP) != 0;
}

void print_trapframe(struct trapframe *tf) {
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr) {
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

//lab3 function2 2513097 杨永琛
static int tick_print_count = 0;

// 判断实验所用的16位压缩指令与32位普通指令：低两位不是11则为2字节，否则为4字节。
// epc 是发生异常的指令地址；推进后，返回时才不会反复执行同一条异常指令。
static inline uintptr_t trap_instruction_len(uintptr_t epc) {
    uint16_t insn = *(uint16_t *)epc;
    return ((insn & 0x3) == 0x3) ? 4 : 2;
}

// interrupt_handler（中断处理）：时钟属于异步事件，不推进 epc。
void interrupt_handler(struct trapframe *tf) {
    // cause 为无符号字段，先左移再右移去掉最高位的中断标志，保留原因编号。
    intptr_t cause = (tf->cause << 1) >> 1;
    switch (cause) {
        case IRQ_S_TIMER:
            // 每收到一次时钟中断计数一次，并预约下一次，否则只会触发一次。
            ticks++;
            clock_set_next_event();
            // 每100次输出一次；累计输出10次后通过 OpenSBI 关机。
            if (ticks % TICK_NUM == 0) {
                print_ticks();
                tick_print_count++;
                if (tick_print_count >= SHUTDOWN_AFTER) {
                    sbi_shutdown();
                }
            }
            break;
        case IRQ_S_SOFT:
            cprintf("Supervisor software interrupt\n");
            break;
        default:
            cprintf("Unknown interrupt: cause %d\n", (int)cause);
            print_trapframe(tf);
            break;
    }
}

// exception_handler（异常处理）：断点与非法指令是同步异常，处理后跳过触发指令。
// 其他异常仅打印现场，这是本实验规定的兜底行为，不代表已经修复异常。
void exception_handler(struct trapframe *tf) {
    // cause 为无符号字段，先左移再右移去掉最高位的中断标志，保留原因编号。
    intptr_t cause = (tf->cause << 1) >> 1;
    switch (cause) {
        case CAUSE_ILLEGAL_INSTRUCTION:
            cprintf("Illegal instruction at 0x%lx\n", tf->epc);
            tf->epc += trap_instruction_len(tf->epc);
            break;
        case CAUSE_BREAKPOINT:
            cprintf("Breakpoint at 0x%lx\n", tf->epc);
            tf->epc += trap_instruction_len(tf->epc);
            break;
        default:
            print_trapframe(tf);
            break;
    }
}

static inline void trap_dispatch(struct trapframe *tf) {
    if ((intptr_t)tf->cause < 0) {
        // 最高位为1：交给中断处理函数。
        interrupt_handler(tf);
    } else {
        // 最高位为0：交给异常处理函数。
        exception_handler(tf);
    }
}

// trap（统一处理入口）：接收汇编传来的现场，处理后返回汇编恢复现场并执行 sret。
void trap(struct trapframe *tf) {
    // 按原因分发；对 tf->epc 的修改会由 RESTORE_ALL 写回 sepc。
    trap_dispatch(tf);
}
