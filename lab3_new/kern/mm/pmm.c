#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <../sync/sync.h>
#include <dtb.h>

// 每个物理页对应一张 Page 描述符，pages 指向描述符数组的虚拟地址。
struct Page *pages;
// npage 是物理页号的排他上界，不是内存页数；描述符数量为 npage - nbase。
size_t npage = 0;
// 内核虚拟地址减去 va_pa_offset，得到对应物理地址。
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// 启动页表的虚拟地址
uintptr_t *satp_virtual = NULL;
// 启动页表的物理地址
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void page_init(void);
static void check_alloc_page(void);

// 初始化物理内存管理器：沿用实验二，默认 Best-Fit，编译时可选择 Buddy。
static void init_pmm_manager(void) {
#ifdef PMM_BUDDY
    // lab2 integration 2512885 李树青：选择伙伴系统，默认保留Best-Fit。
    pmm_manager = &buddy_pmm_manager;
#else
    pmm_manager = &best_fit_pmm_manager;
#endif
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// 将完整空闲页交给所选分配器，建立空闲块链表。
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// 申请连续 n 页：实验三已启用中断，修改分配器链表期间先保存并关闭中断。
// 完成后恢复原来的中断状态；原本关闭时不会被误开启。
struct Page *alloc_pages(size_t n) {
    struct Page *page = NULL;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        page = pmm_manager->alloc_pages(n);
    }
    local_intr_restore(intr_flag);
    return page;
}

// 释放连续页，同样用中断保护保证单核环境下链表更新不被打断。
void free_pages(struct Page *base, size_t n) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        pmm_manager->free_pages(base, n);
    }
    local_intr_restore(intr_flag);
}

// 在中断保护下读取当前空闲页数，保持统计与链表操作一致。
size_t nr_free_pages(void) {
    size_t ret;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        ret = pmm_manager->nr_free_pages();
    }
    local_intr_restore(intr_flag);
    return ret;
}

// lab2 function1 2513387 胡博延
// lab2 function1 revision 2512885 李树青：边界检查、完整页判断及融合修订
// page_init（初始化物理页）：从设备树读取内存范围，在内核末尾布置描述符，
// 保留固件、内核和描述符占用页，再把剩余完整页交给分配器。
static void page_init(void) {
    extern char end[];
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    // 先检查设备树数据，再计算页数或写描述符，防止零大小、地址溢出及错误基址。
    // 框架的地址换算约定 pages[0] 对应 DRAM_BASE。
    if (mem_base != DRAM_BASE || mem_size == 0 ||
        mem_size > (uint64_t)-1 - mem_base) {
        panic("page_init: invalid DTB memory range");
    }
    uint64_t reported_end = mem_base + mem_size;
    uintptr_t mem_end = ROUNDDOWN(
        reported_end < PHYSICAL_MEMORY_END ? reported_end : PHYSICAL_MEMORY_END,
        PGSIZE);
    cprintf("physical memory map:\n");
    cprintf("  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n",
            mem_size, mem_base, reported_end - 1);
    if (mem_end <= KERNEL_BEGIN_PADDR || PADDR((uintptr_t)end) > mem_end) {
        panic("page_init: managed RAM cannot contain the kernel");
    }

    npage = PPN(mem_end); // 物理页号的排他上界。
    uintptr_t pages_va = ROUNDUP((uintptr_t)end, PGSIZE);
    uintptr_t pages_pa = PADDR(pages_va);
    size_t count = npage - nbase;
    // 先用除法判断容量，避免 count * sizeof(Page) 在检查前溢出。
    if (pages_pa > mem_end || count > (mem_end - pages_pa) / sizeof(struct Page)) {
        panic("page_init: no room for Page descriptors");
    }
    pages = (struct Page *)pages_va;
    for (size_t i = 0; i < count; i++) {
        pages[i].flags = 0;
        SetPageReserved(&pages[i]);
        pages[i].property = 0;
        set_page_ref(&pages[i], 0);
        list_init(&pages[i].page_link);
    }

    uintptr_t freemem = ROUNDUP(pages_pa + count * sizeof(struct Page), PGSIZE);
    size_t free_count = freemem < mem_end ? (mem_end - freemem) / PGSIZE : 0;
    // 只有剩余完整页才登记，避免对排他上界调用 pa2page。
    if (free_count > 0) {
        init_memmap(pa2page(freemem), free_count);
    }
    cprintf("  managed end: 0x%016lx, descriptors: %lu\n", mem_end, count);
    cprintf("  free start: 0x%016lx, free pages: %lu\n", freemem, free_count);
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
