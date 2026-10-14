#include "memlayout.h"
#include <assert.h>
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <buddy_pmm.h>
#include <stdio.h>

// WARN: 当 QEMU 没有使用 -m 参数指定内存大小时，它就会默认分配 128 MB 的 RAM
// 所以这里的 MAX_ORDER 值仅仅是对本实验的适配
// 默认最大支持 2^15 页 = 128 MB
#define MAX_ORDER 15   

static free_area_t free_area[MAX_ORDER + 1];
static unsigned int total_free = 0;   // 总空闲页数

// 计算满足 2^order >= n 的最小 order
static int get_order(size_t n) {
    int order = 0;
    while ((1UL << order) < n) {
        order++;
    }
    return order;
}

static void buddy_init(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }
    total_free = 0;
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    // 重置每一页的状态
    for (struct Page *p = base; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    total_free += n;

    // 将连续区间拆分成若干个 2^order 大小的块
    struct Page *p = base;
    size_t remaining = n;
    while (remaining > 0) {
        size_t idx = p - pages;
        int order = 0;

        // 计算当前地址的最大对齐阶
        while ((idx & ((1UL << (order + 1)) - 1)) == 0 &&     // 当前地址是否按 2^(order+1) 页对齐
               (1UL << (order + 1)) <= remaining &&           // 块大小不能超过剩余的页数
               order + 1 <= MAX_ORDER) {                      // 不能超过最大阶
            order++;
        }
        // 取不超过剩余页数的最大阶
        while ((1UL << order) > remaining) order--;

        // 登记这个空闲块
        p->property = (1UL << order);
        SetPageProperty(p);
        list_add(&free_area[order].free_list, &(p->page_link));
        free_area[order].nr_free += (1UL << order);

        p += (1UL << order);
        remaining -= (1UL << order);
    }
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > total_free) return NULL;

    int order = get_order(n);          // 需要的最小阶
    if (order > MAX_ORDER) return NULL;

    // 向上寻找第一个非空的链表
    int cur = order;
    while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list)) cur++;
    if (cur > MAX_ORDER) return NULL;

    // 从该链表取一个块
    list_entry_t *le = list_next(&free_area[cur].free_list);
    struct Page *page = le2page(le, page_link);
    list_del(&(page->page_link));
    free_area[cur].nr_free -= (1UL << cur);
    ClearPageProperty(page);

    // 一路分裂到目标 order
    while (cur > order) {
        cur--;
        struct Page *buddy = page + (1UL << cur);
        buddy->property = (1UL << cur);
        SetPageProperty(buddy);
        list_add(&free_area[cur].free_list, &(buddy->page_link));
        free_area[cur].nr_free += (1UL << cur);
    }

    // 实际分配了 2^order 页，更新总空闲数
    total_free -= (1UL << order);

    return page;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    int order = get_order(n);
    size_t block_size = 1UL << order;

    // 重置实际释放的 block_size 页
    for (struct Page *p = base; p != base + block_size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    total_free += block_size;

    struct Page *page = base;

    // 向上合并伙伴
    while (order < MAX_ORDER) {
        // Buddy 的特性：两个伙伴块的索引只有第 order 位不同
        size_t idx = page - pages;
        size_t buddy_idx = idx ^ (1UL << order);

        // 伙伴必须在有效范围内
        if (buddy_idx >= (size_t)(npage - nbase)) break;

        struct Page *buddy = pages + buddy_idx;

        // 伙伴必须是空闲块头，且大小匹配
        if (!PageProperty(buddy) || buddy->property != (1UL << order)) break;

        // 从链表中删除伙伴
        list_del(&(buddy->page_link));
        free_area[order].nr_free -= (1UL << order);
        ClearPageProperty(buddy);

        // 合并：取地址较小的作为新块头
        if (buddy < page) page = buddy;
        order++;
    }

    // 将合并后的块挂到对应 order 的链表
    page->property = (1UL << order);
    SetPageProperty(page);
    list_add(&free_area[order].free_list, &(page->page_link));
    free_area[order].nr_free += (1UL << order);
}

static size_t buddy_nr_free_pages(void) {
    return total_free;
}

// =======================================================
// 下面是与测试相关的函数


// 保存当前状态，清空所有链表
static unsigned int
clear_area(free_area_t free_area_store[]) { // 务必保证free_area_store 的长度是 MAX_ORDER + 1
    // 1. 保存当前状态
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_area_store[i] = free_area[i];
    }
    unsigned int total_free_store = total_free;

    // 2. 清空所有阶的空闲链表
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }
    total_free = 0;

    for (size_t i = 0; i < npage - nbase; i++) {
        ClearPageProperty(pages + i);
    }

    return total_free_store;
}

// 恢复之前的状态
static void
restore_area(free_area_t free_area_store[], unsigned int total_free_store) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_area[i] = free_area_store[i];
    }
    total_free = total_free_store;

    for (int i = 0; i <= MAX_ORDER; i++) {
        list_entry_t *le = &free_area[i].free_list;
        while ((le = list_next(le)) != &free_area[i].free_list) {
            struct Page *p = le2page(le, page_link);
            SetPageProperty(p);
        }
    }
}

// 基础检查：测试分配、释放、空闲计数
static void
buddy_basic_check(void) {
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;

    // 1. 分配 3 个 1 页块
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);

    // 2. 引用计数应为 0，物理地址应合法
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);
    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    // 3. 分配出去的块不应是空闲块头
    assert(!PageProperty(p0));
    assert(!PageProperty(p1));
    assert(!PageProperty(p2));

    // 4. 保存并清空状态（模拟内存耗尽的情景)
    free_area_t free_area_store[MAX_ORDER + 1];
    unsigned int total_free_store = clear_area(free_area_store);

    // 5. 空闲页为 0，分配应失败
    assert(alloc_page() == NULL);

    // 6. 释放 3 页，空闲页数应为 3
    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(total_free == 3);
    assert(buddy_nr_free_pages() == 3);

    // 7. 重新分配 3 页，应成功
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    // 8. 空闲页耗尽，再分配应失败
    assert(alloc_page() == NULL);

    // 9. 释放 p0，应能重新分配到同一页（LIFO 行为在 buddy 中不一定保证，
    //    但分配应成功）
    free_page(p0);
    assert(total_free == 1);

    struct Page *p;
    assert((p = alloc_page()) != NULL);
    assert(alloc_page() == NULL);

    assert(total_free == 0);

    // 10. 恢复状态
    restore_area(free_area_store, total_free_store);

    // 11. 释放剩余块
    free_page(p);
    free_page(p1);
    free_page(p2);
}

// 链表自洽性检查(check 的开头和结尾都调用一次)
static void
buddy_check_consistency(void) {
    size_t total = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        size_t sum = 0;
        list_entry_t *le = &free_area[i].free_list;
        while ((le = list_next(le)) != &free_area[i].free_list) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == (1UL << i));
            sum += p->property;
        }
        assert(sum == free_area[i].nr_free);
        total += sum;
    }
    assert(total == total_free);
    assert(total == buddy_nr_free_pages());
}

// 检查合并功能
static void
check_coalesce(void) {
    // 先拿一个合法的 2 页块（此时 free_area 还是正常状态）
    struct Page *p2 = alloc_pages(2);
    assert(p2 != NULL);

    // 再保存并清空 free_area
    free_area_t free_area_store[MAX_ORDER + 1];
    unsigned int total_free_store = clear_area(free_area_store);

    // 手动把 p2 挂回 free_area[1]
    p2->flags = 0;                 // 保险起见，清一遍
    p2->property = 2;
    SetPageProperty(p2);
    set_page_ref(p2, 0);
    list_add(&free_area[1].free_list, &(p2->page_link));
    free_area[1].nr_free += 2;
    total_free += 2;

    // 两次分配 1 页
    struct Page *a = alloc_pages(1);
    struct Page *b = alloc_pages(1);
    assert(a && b);
    assert((a == p2 && b == p2 + 1) || (a == p2 + 1 && b == p2));

    // 释放，验证合并
    free_pages(a, 1);
    free_pages(b, 1);
    assert(PageProperty(p2));
    assert(p2->property == 2);

    // 恢复
    restore_area(free_area_store, total_free_store);

    ClearPageProperty(p2);
    free_pages(p2, 2);   // 把 p2 归还给系统
}

// 检查分裂功能
static void
check_split(void) {
    // 1. 先拿一个合法的 8 页块（此时 free_area 还是正常状态）
    struct Page *p8 = alloc_pages(8);
    assert(p8 != NULL);

    // 2. 保存并清空 free_area
    free_area_t free_area_store[MAX_ORDER + 1];
    unsigned int total_free_store = clear_area(free_area_store);

    // 3. 手动把 p8 挂回 free_area[3]
    p8->flags = 0;
    p8->property = 8;
    SetPageProperty(p8);
    set_page_ref(p8, 0);
    list_add(&free_area[3].free_list, &(p8->page_link));
    free_area[3].nr_free += 8;
    total_free += 8;

    // 4. 分配 1 页，触发分裂
    struct Page *p1 = alloc_pages(1);
    assert(p1 != NULL);
    assert(p1 == p8);          // 应该正是 p8 所在的块

    // 5. 验证 p8 从 free_area[3] 消失
    bool found8 = 0;
    list_entry_t *le = &free_area[3].free_list;
    while ((le = list_next(le)) != &free_area[3].free_list) {
        struct Page *q = le2page(le, page_link);
        if (q == p8) { found8 = 1; break; }
    }
    assert(!found8);

    // 6. 验证分裂出的三块：4 页、2 页、1 页
    //    分裂顺序：8 页 → 保留前 4 页，后 4 页挂到 free_area[2]
    //              → 保留前 2 页，后 2 页挂到 free_area[1]
    //              → 保留前 1 页，后 1 页挂到 free_area[0]
    struct Page *p4  = p8 + 4;   // 4 页块头，应在 free_area[2]
    struct Page *p2  = p8 + 2;   // 2 页块头，应在 free_area[1]
    struct Page *p1b = p8 + 1;   // 1 页块头，应在 free_area[0]

    assert(PageProperty(p4)  && p4->property  == 4);
    assert(PageProperty(p2)  && p2->property  == 2);
    assert(PageProperty(p1b) && p1b->property == 1);

    // 7. 验证它们在正确的阶里（链表可达）
    bool found4 = 0, found2 = 0, found1 = 0;
    le = &free_area[2].free_list;
    while ((le = list_next(le)) != &free_area[2].free_list) {
        struct Page *q = le2page(le, page_link);
        if (q == p4) { found4 = 1; break; }
    }
    le = &free_area[1].free_list;
    while ((le = list_next(le)) != &free_area[1].free_list) {
        struct Page *q = le2page(le, page_link);
        if (q == p2) { found2 = 1; break; }
    }
    le = &free_area[0].free_list;
    while ((le = list_next(le)) != &free_area[0].free_list) {
        struct Page *q = le2page(le, page_link);
        if (q == p1b) { found1 = 1; break; }
    }
    assert(found4 && found2 && found1);

    // 8. 恢复链表
    restore_area(free_area_store, total_free_store);

    // 9. 清理 p8 分裂时留下的页状态，避免归还时误合并
    //    （p8+1, p8+2, p8+4 现在是幽灵，带着 PG_property 但不可达）
    ClearPageProperty(p8);
    ClearPageProperty(p8 + 1);
    ClearPageProperty(p8 + 2);
    ClearPageProperty(p8 + 4);

    // 10. 归还 p8
    free_pages(p8, 1);
}

// 检查非 2 的幂次请求：请求 3 页应分配 4 页
static void
check_non_power_of_two(void) {
    // 1. 先拿一个合法的 4 页块
    struct Page *p4 = alloc_pages(4);
    assert(p4 != NULL);

    // 2. 保存并清空 free_area
    free_area_t free_area_store[MAX_ORDER + 1];
    unsigned int total_free_store = clear_area(free_area_store);

    // 3. 手动把 p4 挂回 free_area[2]
    p4->flags = 0;
    p4->property = 4;
    SetPageProperty(p4);
    set_page_ref(p4, 0);
    list_add(&free_area[2].free_list, &(p4->page_link));
    free_area[2].nr_free += 4;
    total_free += 4;

    // 4. 记录分配前的空闲页数
    size_t before_free = total_free;

    // 5. 请求 3 页（非 2 的幂），get_order(3)=2，实际应分配 4 页
    struct Page *p3 = alloc_pages(3);
    assert(p3 != NULL);

    // 6. 验证：
    //    - 返回的块应该正是 p4（唯一可用的 4 页块）
    assert(p3 == p4);

    //    - 块起始地址按 4 页对齐（order=2 要求）
    size_t idx = p3 - pages;
    assert((idx & 0x3) == 0);

    //    - 实际消耗了 4 页，而不是 3 页
    assert(total_free == before_free - 4);

    //    - 分配出去的块不再是空闲块头
    assert(!PageProperty(p3));

    //    - p3 + 1 / p3 + 2 / p3 + 3 也不是空闲块头
    assert(!PageProperty(p3 + 1));
    assert(!PageProperty(p3 + 2));
    assert(!PageProperty(p3 + 3));

    //    - free_area[2] 为空（唯一块被取走）
    assert(list_empty(&free_area[2].free_list));
    assert(free_area[2].nr_free == 0);

    // 7. 再请求一次 3 页，应该失败（已经没有足够的块了）
    assert(alloc_pages(3) == NULL);

    // 8. 恢复链表
    restore_area(free_area_store, total_free_store);

    // 9. 清理 p3 所在块的残留状态，归还
    ClearPageProperty(p3);
    ClearPageProperty(p3 + 1);
    ClearPageProperty(p3 + 2);
    ClearPageProperty(p3 + 3);
    free_pages(p3, 4);   // 按实际大小 4 页归还
}


// 完整检查：基础检查 + 合并 + 分裂 + 边界
static void
buddy_check(void) {
    buddy_check_consistency();

    // 1. 初始状态：所有阶空闲页数之和等于 total_free
    size_t total = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        total += free_area[i].nr_free;
    }
    assert(total == total_free);
    assert(total == buddy_nr_free_pages());

    // 2. 基础检查
    buddy_basic_check();

    // 3. 测试不同阶的分配
    struct Page *p1 = alloc_pages(1);
    struct Page *p2 = alloc_pages(2);
    struct Page *p4 = alloc_pages(4);
    struct Page *p8 = alloc_pages(8);
    assert(p1 && p2 && p4 && p8);

    // 分配出去的块头不应有 PG_property
    assert(!PageProperty(p1));
    assert(!PageProperty(p2));
    assert(!PageProperty(p4));
    assert(!PageProperty(p8));

    // 4. 释放它们，空闲页应恢复
    free_pages(p1, 1);
    free_pages(p2, 2);
    free_pages(p4, 4);
    free_pages(p8, 8);
    assert(total_free == buddy_nr_free_pages());

    // 5. 测试伙伴合并
    check_coalesce();

    // 6. 测试分裂
    check_split();

    // 7. 测试非 2 的幂次请求
    check_non_power_of_two();

    // 8. 测试分配失败：请求超过 MAX_ORDER 的块
    struct Page *huge = alloc_pages(1UL << (MAX_ORDER + 1));
    assert(huge == NULL);

    // 9. 测试空闲状态：当前空闲页数应与初始一致
    size_t final_total = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        final_total += free_area[i].nr_free;
    }
    assert(final_total == total_free);
    assert(final_total == buddy_nr_free_pages());

    buddy_check_consistency();
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
