#include <buddy_pmm.h>
#include <list.h>

//lab2 function2-3 2512885 李树青
// BUDDY IMPLEMENTATION: separate from Best-Fit and its framework tests.
static list_entry_t order_lists[BUDDY_MAX_ORDER + 1];
static size_t free_count;
#define ALLOC_HEAD (1UL << 2)
#define ALLOC_TAIL (1UL << 3)
#define MAX_PAGES ((PHYSICAL_MEMORY_END - DRAM_BASE) / PGSIZE)
static unsigned char seen[MAX_PAGES];

static unsigned int order_for(size_t n) {
    unsigned int order = 0;
    assert(n > 0 && n <= (1UL << BUDDY_MAX_ORDER));
    while ((1UL << order) < n) order++;
    return order;
}

static void add_block(struct Page *base, unsigned int order) {
    base->property = 1UL << order;
    SetPageProperty(base);
    list_add(&order_lists[order], &base->page_link);
}

static void merge_block(struct Page *base, unsigned int order) {
    while (order < BUDDY_MAX_ORDER) {
        // XOR selects the other half of the same physically aligned parent.
        size_t partner_ppn = page2ppn(base) ^ (1UL << order);
        if (partner_ppn < nbase || partner_ppn + (1UL << order) > npage) break;
        struct Page *partner = pages + partner_ppn - nbase;
        if (!PageProperty(partner) || partner->property != (1UL << order)) break;
        list_del(&partner->page_link);
        ClearPageProperty(partner);
        partner->property = 0;
        if (partner < base) base = partner;
        order++;
    }
    add_block(base, order);
}

static void buddy_init(void) {
    for (unsigned int i = 0; i <= BUDDY_MAX_ORDER; i++) list_init(&order_lists[i]);
    free_count = 0;
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0 && base >= pages && base + n <= pages + npage - nbase);
    for (struct Page *p = base; p < base + n; p++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
        list_init(&p->page_link);
    }
    free_count += n;
    // Cover arbitrary initial extents with aligned blocks, losing no whole pages.
    while (n > 0) {
        unsigned int order = BUDDY_MAX_ORDER;
        while ((1UL << order) > n || (page2ppn(base) & ((1UL << order) - 1))) order--;
        merge_block(base, order);
        base += 1UL << order;
        n -= 1UL << order;
    }
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > (1UL << BUDDY_MAX_ORDER)) return NULL;
    unsigned int wanted = order_for(n), order = wanted;
    size_t actual = 1UL << wanted;
    if (actual > free_count) return NULL;
    while (order <= BUDDY_MAX_ORDER && list_empty(&order_lists[order])) order++;
    if (order > BUDDY_MAX_ORDER) return NULL;
    struct Page *base = le2page(list_next(&order_lists[order]), page_link);
    list_del(&base->page_link);
    ClearPageProperty(base);
    base->property = 0;
    while (order > wanted) {
        order--;
        add_block(base + (1UL << order), order);
    }
    for (size_t i = 0; i < actual; i++) {
        base[i].flags = ALLOC_TAIL;
        base[i].property = 0;
    }
    base->flags = ALLOC_HEAD;
    base->property = n;
    free_count -= actual;
    return base;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0 && n <= (1UL << BUDDY_MAX_ORDER));
    assert(base >= pages && base < pages + npage - nbase);
    assert(base->flags == ALLOC_HEAD && base->property == n);
    unsigned int order = order_for(n);
    size_t actual = 1UL << order;
    assert(base + actual <= pages + npage - nbase);
    assert((page2ppn(base) & (actual - 1)) == 0);
    for (size_t i = 1; i < actual; i++) assert(base[i].flags == ALLOC_TAIL);
    for (size_t i = 0; i < actual; i++) {
        base[i].flags = base[i].property = 0;
        set_page_ref(&base[i], 0);
    }
    free_count += actual;
    merge_block(base, order);
}

static size_t buddy_nr_free_pages(void) {
    return free_count;
}

void buddy_validate(void) {
    size_t total = 0, count = npage - nbase;
    assert(count <= MAX_PAGES);
    for (size_t i = 0; i < count; i++) seen[i] = 0;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        list_entry_t *head = &order_lists[order];
        for (list_entry_t *le = list_next(head); le != head; le = list_next(le)) {
            struct Page *p = le2page(le, page_link);
            size_t size = 1UL << order;
            assert(p >= pages && p + size <= pages + count);
            assert(p->flags == (1UL << PG_property) && p->property == size);
            assert((page2ppn(p) & (size - 1)) == 0);
            assert(list_next(list_prev(le)) == le && list_prev(list_next(le)) == le);
            for (size_t i = 0; i < size; i++) {
                size_t index = p - pages + i;
                assert(!seen[index] && !PageReserved(&p[i]));
                if (i > 0) assert(p[i].flags == 0);
                seen[index] = 1;
            }
            if (order < BUDDY_MAX_ORDER) {
                size_t ppn = page2ppn(p) ^ size;
                if (ppn >= nbase && ppn + size <= npage) {
                    struct Page *partner = pages + ppn - nbase;
                    assert(!PageProperty(partner) || partner->property != size);
                }
            }
            total += size;
            assert(total <= count);
        }
    }
    assert(total == free_count);
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
