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
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// Exclusive upper bound of managed physical page numbers (not the RAM page count).
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void page_init(void);
static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
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

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

// lab2 function1 2513387 胡博延
// lab2 function1 revision 2512885 李树青：边界检查、完整页判断及融合修订
// page_init - probe physical memory from the device tree, layout the Page
// descriptor array after the kernel image, and hand the free memory over to
// the current pmm manager.
static void page_init(void) {
    extern char end[];
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    // Reject invalid input BEFORE computing page counts or writing descriptors.
    // The conversion helpers assume pages[0] describes DRAM_BASE.
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

    npage = PPN(mem_end); // Exclusive physical page-number bound, not a count.
    uintptr_t pages_va = ROUNDUP((uintptr_t)end, PGSIZE);
    uintptr_t pages_pa = PADDR(pages_va);
    size_t count = npage - nbase;
    // Division avoids overflowing count*sizeof(Page) before the bounds check.
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
    // In particular, do not call pa2page at the exclusive end boundary.
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
