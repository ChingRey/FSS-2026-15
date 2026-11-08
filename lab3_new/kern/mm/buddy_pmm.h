#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

// lab2 function2-3-interface 2512885 李树青

// 伙伴系统实现位于 buddy_pmm.c，独立自检位于 buddy_check.c。
// 块大小为2的order次幂页，非2次幂请求向上取整。
// 释放必须传原始块头和原始请求页数，不支持部分释放；非法释放触发断言。
#define BUDDY_MAX_ORDER 15
extern const struct pmm_manager buddy_pmm_manager;
void buddy_check(void);
void buddy_validate(void); // 供独立测试调用的不变量检查。

#endif
