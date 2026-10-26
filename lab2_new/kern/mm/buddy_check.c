#include <buddy_pmm.h>
#include <stdio.h>

//lab2 function2-3-test 2512885 李树青
// BUDDY TESTS: independent translation unit; never alter the Best-Fit tests.
static struct Page *held[(PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE];

static uintptr_t *payload(struct Page *p) {
    return (uintptr_t *)(page2pa(p) + va_pa_offset);
}

// Negative tests deliberately panic, so select ONE in a separate QEMU boot.
static void negative_check(void) {
#if defined(BUDDY_TEST_ZERO)
    alloc_pages(0);
    panic("Buddy negative test unexpectedly returned");
#elif defined(BUDDY_TEST_DOUBLE)
    struct Page *p = alloc_page();
    assert(p != NULL);
    free_page(p);
    free_page(p);
    panic("Buddy negative test unexpectedly returned");
#elif defined(BUDDY_TEST_LENGTH)
    struct Page *p = alloc_pages(3);
    assert(p != NULL);
    free_pages(p, 4);
    panic("Buddy negative test unexpectedly returned");
#elif defined(BUDDY_TEST_INTERIOR)
    struct Page *p = alloc_pages(2);
    assert(p != NULL);
    free_page(p + 1);
    panic("Buddy negative test unexpectedly returned");
#endif
}

void buddy_check(void) {
    negative_check();
    size_t before = nr_free_pages();
    buddy_validate();
    struct Page *arena = alloc_pages(8);
    assert(arena != NULL && (page2ppn(arena) & 7) == 0);

    // Hold all other pages, so they cannot mask failures in the controlled arena.
    size_t count = 0;
    struct Page *p;
    while ((p = alloc_page()) != NULL) {
        assert(count < sizeof(held) / sizeof(held[0]));
        held[count] = p;
        payload(p)[0] = count;
        payload(p)[PGSIZE / sizeof(uintptr_t) - 1] = ~count;
        count++;
    }
    assert(nr_free_pages() == 0 && alloc_pages(2) == NULL);
    assert(count + 8 == before);
    cprintf("[Buddy] exhaustion and failed allocation: PASS\n");

    free_pages(arena, 8);
    struct Page *a = alloc_page(), *b = alloc_page();
    assert(a == arena && b == arena + 1);
    assert(nr_free_pages() == 6 && alloc_pages(8) == NULL);
    free_page(a);
    free_page(b);
    buddy_validate();
    assert((a = alloc_pages(8)) == arena);
    free_pages(a, 8);
    cprintf("[Buddy] split, alignment and recursive merge: PASS\n");

    struct Page *small[8];
    for (size_t i = 0; i < 8; i++) {
        small[i] = alloc_page();
        assert(small[i] == arena + i);
    }
    free_page(small[1]);
    free_page(small[2]);
    assert(nr_free_pages() == 2 && alloc_pages(2) == NULL);
    buddy_validate();
    for (size_t i = 0; i < 8; i++) {
        if (i != 1 && i != 2) free_page(small[i]);
    }
    a = alloc_pages(3);
    b = alloc_pages(4);
    assert(a == arena && b == arena + 4 && nr_free_pages() == 0);
    payload(a)[0] = 0x13579bdf;
    payload(a + 2)[PGSIZE / sizeof(uintptr_t) - 1] = 0x11223344;
    payload(b)[0] = 0x2468ace0;
    assert(payload(a)[0] == 0x13579bdf && payload(b)[0] == 0x2468ace0);
    assert(payload(a + 2)[PGSIZE / sizeof(uintptr_t) - 1] == 0x11223344);
    free_pages(a, 3); // Original request length; four pages are actually returned.
    free_pages(b, 4);
    buddy_validate();
    cprintf("[Buddy] non-partners, rounding and payloads: PASS\n");

    // All non-arena pages are held. Reinitialize only this controlled test pool.
    pmm_manager->init();
    for (size_t i = 0; i < 8; i++) {
        arena[i].flags = 0;
        SetPageReserved(&arena[i]);
        arena[i].property = 0;
    }
    pmm_manager->init_memmap(arena + 1, 5);
    assert(nr_free_pages() == 5 && alloc_pages(8) == NULL);
    size_t visited = 0;
    for (size_t i = 0; i < 5; i++) {
        small[i] = alloc_page();
        assert(small[i] >= arena + 1 && small[i] < arena + 6);
        size_t bit = 1UL << (small[i] - arena);
        assert((visited & bit) == 0);
        visited |= bit;
    }
    assert(visited == 0x3e && alloc_page() == NULL && nr_free_pages() == 0);
    for (size_t i = 0; i < 5; i++) free_page(small[i]);
    buddy_validate();
    assert(nr_free_pages() == 5 && alloc_pages(8) == NULL);
    assert(PageReserved(arena) && PageReserved(arena + 6) && PageReserved(arena + 7));

    // Restore the original arena, then return every outside allocation.
    pmm_manager->init();
    for (size_t i = 0; i < 8; i++) {
        arena[i].flags = 0;
        SetPageReserved(&arena[i]);
        arena[i].property = 0;
    }
    pmm_manager->init_memmap(arena, 8);
    cprintf("[Buddy] unaligned five-page initial extent: PASS\n");
    while (count > 0) {
        count--;
        p = held[count];
        assert(payload(p)[0] == count);
        assert(payload(p)[PGSIZE / sizeof(uintptr_t) - 1] == ~count);
        free_page(p);
    }
    assert(nr_free_pages() == before);
    assert(alloc_pages((1UL << BUDDY_MAX_ORDER) + 1) == NULL);
    buddy_validate();
    cprintf("[Buddy] full recovery and invariants: PASS (%lu pages)\n", before);
}
