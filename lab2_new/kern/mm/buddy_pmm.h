#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

// lab2 function2-3-interface 2512885 李树青

// TASK 3: implementation is buddy_pmm.c; independent tests are buddy_check.c.
// Blocks hold 2^order pages. Non-power-of-two requests round UP.
// Free the ORIGINAL allocation head with the ORIGINAL requested page count.
// Partial frees are unsupported. Invalid frees assert instead of corrupting lists.
#define BUDDY_MAX_ORDER 15
extern const struct pmm_manager buddy_pmm_manager;
void buddy_check(void);
void buddy_validate(void); // Internal invariant check used by the test module.

#endif
