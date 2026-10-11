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

    // 4. 保存并清空空闲状态
    //    Buddy 有多个链表，需要逐个保存和恢复
    free_area_t free_area_store[MAX_ORDER + 1];
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_area_store[i] = free_area[i];
    }
    unsigned int total_free_store = total_free;

    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }
    total_free = 0;

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

    // 10. 恢复空闲状态
    for (int i = 0; i <= MAX_ORDER; i++) {
        free_area[i] = free_area_store[i];
    }
    total_free = total_free_store;

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

    // 5. 测试伙伴合并：分配两个 1 页块，释放后应能合并成 2 页
    struct Page *a = alloc_pages(1);
    struct Page *b = alloc_pages(1);
    assert(a && b);
    free_pages(a, 1);
    free_pages(b, 1);
    // 合并后应能分配 2 页
    struct Page *p2_merged = alloc_pages(2);
    assert(p2_merged != NULL);
    free_pages(p2_merged, 2);

    // 6. 测试分裂：分配 8 页块，释放后再分配 1 页
    struct Page *p8_2 = alloc_pages(8);
    assert(p8_2 != NULL);
    free_pages(p8_2, 8);
    struct Page *s1 = alloc_pages(1);
    struct Page *s2 = alloc_pages(1);
    assert(s1 && s2);
    free_pages(s1, 1);
    free_pages(s2, 1);

    // 7. 测试非 2 的幂次请求：请求 3 页应返回 4 页的块
    struct Page *p3 = alloc_pages(3);
    assert(p3 != NULL);
    // 该块实际上是 4 页，释放时也按 4 页归还
    free_pages(p3, 4);

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
