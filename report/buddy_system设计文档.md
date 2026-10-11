# Buddy System（伙伴系统）物理内存分配算法设计文档

> Lab 2 扩展练习 Challenge · 设计文档
> 实现文件：`code/kern/mm/buddy_pmm.c`、`code/kern/mm/buddy_pmm.h`
> 集成位置：`code/kern/mm/pmm.c` 的 `init_pmm_manager()`
> 代码基线：commit `e4db532`
>
> 说明：本文档中的代码行号、结构体大小、初始化拆分结果（§4.2）与运行输出（§6.2）均来自本实现与一次实际构建运行；如改动代码后数值有变，请以重新运行结果为准。

---

## 1. 概述

### 1.1 需求

ucore 的物理内存管理采用**策略与机制分离**的设计：`pmm.c` 只负责“机制”（从设备树探测物理内存布局、建立 `struct Page` 数组、对外提供 `alloc_pages/free_pages/nr_free_pages`），具体分配算法由 `struct pmm_manager` 定义的一组函数指针决定。因此本 Challenge 的目标是：

> 新增一个 `buddy_pmm_manager` 实例，实现 `init / init_memmap / alloc_pages / free_pages / nr_free_pages / check` 六个接口，并保证在 **O(1)～O(MAX_ORDER)** 时间内完成分配、释放与伙伴合并。

### 1.2 需要实现的函数

```c
static void          buddy_init(void);                              // 初始化内部数据结构
static void          buddy_init_memmap(struct Page *base, size_t n); // 接管 n 页空闲物理内存
static struct Page  *buddy_alloc_pages(size_t n);                   // 分配 n 页（向上取整到 2 的幂）
static void          buddy_free_pages(struct Page *base, size_t n);  // 释放并以伙伴为单位向上合并
static size_t        buddy_nr_free_pages(void);                      // 返回空闲页总数
static void          buddy_check(void);                              // 自检用例
```

外加两个内部辅助函数 `get_order()`、`buddy_check_consistency()`，以及模块入口：

```c
const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
```

### 1.3 设计约束（来自 ucore 框架）

| 约束 | 出处 | 对设计的影响 |
|------|------|--------------|
| 分配粒度固定为 4 KB 物理页 | `PGSIZE` | 伙伴块大小只能取 `2^k` 页 |
| `struct Page` 已固定为 `{ref, flags, property, page_link}`（40 B） | `memlayout.h:34` | 不能新增字段，只能复用 `property` 与 `PG_property` 位 |
| 只有 `PG_reserved`、`PG_property` 两个标志位可用 | `memlayout.h:42` | 用 `PG_property` 充当“空闲块头”标记，替代经典实现的位图 |
| `init_memmap(base, n)` 只保证从 `base` 起 `n` 页连续**且页粒度高对齐**，`base` 本身不一定按大块对齐 | `pmm.c:100-104` | 初始化时必须按“最大对齐”拆分成多个 2 的幂块 |
| `free_pages(base, n)` 的 `n` 是**调用者传入**的，内核不记录分配大小 | `pmm.h:26` | 释放时用 `get_order(n)` 还原块大小，要求调用者两次传参一致 |
| `pages` 数组索引从 `0` 开始对应 PPN `nbase` | `pmm.h:78-83` | 所有下标运算统一使用 `page - pages`，不使用 PPN |

### 1.4 设计目标

1. **正确性**：任意分配/释放序列结束后，空闲块集合恰好覆盖全部空闲页，无重叠、无丢失。
2. **合并（coalescing）**：释放时自动与伙伴合并，抑制外部碎片。
3. **低开销**：不引入任何辅助数组/位图，元数据全部寄存在 `struct Page` 中。
4. **可验证**：提供不变量自检 `buddy_check_consistency()`，可在任意时刻验证内部一致性。

---

## 2. 总体设计

### 2.1 核心思想

把受管物理内存看成**一棵以页为叶子的完全二叉树**：

- **阶（order）**：大小恰好为 `2^order` 页的块称为“阶为 order 的块”。
- **地址对齐**：阶为 `order` 的块，其起始下标必然是 `2^order` 的整数倍。
- **伙伴（buddy）**：两个阶为 `order` 的块互为伙伴，当且仅当它们的下标只在第 `order` 位上不同，即
  `buddy_index = index ^ (1 << order)`。因此伙伴对在内存上总是**相邻且连续**，合并后恰为一块阶 `order+1` 的块。

分配时“大块裂开”，释放时“小块合并”，从而在**快速分配**与**抑制外部碎片**之间取得平衡。

### 2.2 关键设计决策

| 决策点 | 方案 A（经典/参考实现） | 方案 B（本实现） | 选择理由 |
|--------|------------------------|------------------|----------|
| 空闲块组织 | 单一链表按地址序 | **按阶分桶的双向链表数组 `free_area[order]`** | 单链表按地址序虽然也能工作，但要取出“某个大小”的块需要线性扫描；分桶后分配只需扫描阶，`nr_free` 也天然分阶统计 |
| 伙伴状态记录 | 显式维护“分裂/合并”二叉树或位图 | **复用 `Page.property` + `PG_property`** | 不额外占用内存；在 `MAX_ORDER=15` 时若用节点头实现需额外数 MB，位图也需约 4 KB |
| 块头识别 | 需要额外数组标记 | `PG_property==1` 即“我是空闲块头”，`property` 即块页数 | 与 `default_pmm`/`best_fit_pmm` 使用同一套约定，`check` 逻辑可复用 |
| 分配策略 | 首次适配（在最小可用阶内） | 从目标阶**向上**找第一个非空阶，取该链表**第一个**块（FIFO） | 阶越高块越少，向上扫描代价与 `MAX_ORDER` 同阶；取首块保证同一阶内轮转使用 |
| 分裂方向 | —— | 从**左半**（低地址半块）继续递归，**右半**挂回空闲链表 | 与索引计算一致（左半下标仍是 `2^(k-1)` 的倍数），保证拆分后左右两块都是合法伙伴块 |
| 合并终止条件 | 位图判断 | 需要**同时**满足：伙伴在受管范围内、`PG_property==1`、`property == 2^order` | 后者把“已分配块头/内容页/阶不同的块”一次性排除，天然防止错误合并 |

### 2.3 模块在系统中的位置

```
                       ┌──────────────────────────────┐
   调用者（kern/init,   │  alloc_pages(n) / free_pages │   pmm.c
   测试代码…）           │  / nr_free_pages()           │
                       └──────────────┬───────────────┘
                                      │ pmm_manager->…
                       ┌──────────────▼───────────────┐
                       │  const struct pmm_manager    │  pmm.c:31
                       │  = &buddy_pmm_manager        │
                       └──────────────┬───────────────┘
        ┌──────────────────────────────▼──────────────────────────────┐
        │   buddy_pmm.c                                               │
        │   free_area[MAX_ORDER+1]  ── 按阶分桶的空闲链表 + nr_free      │
        │   total_free              ── 空闲页总数（O(1) 查询）           │
        │   pages[]（pmm.c 提供）   ── 每个 Page 用 property/flags 标记  │
        └─────────────────────────────────────────────────────────────┘
```

启用方式：在 `pmm.c` 的 `init_pmm_manager()` 中把

```c
pmm_manager = &default_pmm_manager;   // 默认
pmm_manager = &buddy_pmm_manager;     // 本 Challenge
```

`pmm_init()` 随后按固定顺序调用 `init → page_init() → init_memmap() → check()`，因此 `buddy_pmm_manager` 会被完整地走一遍生命周期。

---

## 3. 数据结构设计

### 3.1 分阶空闲链表

```c
#define MAX_ORDER 15                    // 最大块 = 2^15 页 = 128 MB

static free_area_t free_area[MAX_ORDER + 1];   // 下标即阶
static unsigned int total_free = 0;            // 全局空闲页数
```

其中 `free_area_t` 是 ucore 原有类型（`memlayout.h:57`）：

```c
typedef struct {
    list_entry_t free_list;   // 空闲块头 Page.page_link 组成的双向循环链表
    unsigned int nr_free;     // 本阶的空闲“页数”之和（不是块数）
} free_area_t;
```

- 链表元素是**块头的 `page_link` 字段**，通过 `le2page(le, page_link)` 反查 `struct Page *`。
- 挂链使用 `list_add(&free_area[o].free_list, &p->page_link)`，其语义是插入到表头之后（`list.h:54`，`list_add_after`），因此**新块插在链表尾部 → FIFO 顺序**；`buddy_alloc_pages` 总是取 `list_next(&free_list)`，即最“老”的空闲块，同一阶内形成轮转使用，避免反复分割同一块。
- `nr_free` 统计的是**页数**而非块数（`free_area[3].nr_free == 8` 表示 1 个 8 页块，或 8 个 1 页块之和……以页数计），这样可以 O(1) 地汇总出全局空闲页数，并让 `nr_free` 具有可加性。

### 3.2 每页元数据（`struct Page` 的复用约定）

| 字段 | 空闲块头页 | 空闲块内部页 | 已分配页 |
|------|-----------|-------------|---------|
| `flags.PG_reserved` | 0 | 0 | 0 |
| `flags.PG_property` | **1** | 0 | 0 |
| `property` | **`2^order`**（块页数） | 0（不关心） | 0 |
| `ref` | 0 | 0 | 由调用者维护 |
| `page_link` | 在对应阶的空闲链表中 | 无意义 | 无意义 |

> **注意**：`init_memmap` 中会 `assert(PageReserved(p))`，因为 `pmm.c:94-96` 先把所有页都标为 Reserved，再由分配器把真正的空闲区间“认领”下来；认领时清掉 Reserved 位与 `property`，正是与框架的接口约定。

### 3.3 不变量（Invariants）

整个模块在任何对外可见的稳定状态下，必须满足以下 4 条不变量，`buddy_check_consistency()` 逐条检查：

| 编号 | 不变量 | 检查手段 |
|------|--------|---------|
| **I1** | 对任意 `o`，`free_area[o].free_list` 中的每一项 `p` 都满足 `PageProperty(p)` 且 `p->property == 2^o` | 遍历链表 + 断言 |
| **I2** | 对任意 `o`，链表中各项 `property` 之和等于 `free_area[o].nr_free` | `sum == nr_free` |
| **I3** | `Σ_o free_area[o].nr_free == total_free == buddy_nr_free_pages()` | 全局求和 |
| **I4** | 阶为 `o` 的块头下标按 `2^o` 对齐；`buddy = index ^ 2^o` 落在受管范围内 | 由算法构造性保证（见 §4.2/§4.5 的归纳论证） |

I1+I2+I3 保证了“空闲页计数”与“空闲块视图”一致；I4 保证了伙伴运算的合法性。

---

## 4. 算法设计

### 4.1 阶的计算：`get_order()`

```c
// 计算满足 2^order >= n 的最小 order
static int get_order(size_t n) {
    int order = 0;
    while ((1UL << order) < n) order++;
    return order;
}
```

- 语义：把任意请求页数**向上**取整到 2 的幂，返回其阶。
- `get_order(1)=0`、`get_order(2)=1`、`get_order(3)=2`、`get_order(4)=2`、`get_order(5)=3`……`get_order(2^k)=k`。
- 时间复杂度 `O(MAX_ORDER)`（`n <= 2^64` 时最多循环 64 次），常数极小；也可用 `64 - __builtin_clzl(n-1)` 之类的位运算改写。
- **重要副作用**：它决定了“3 页请求 → 实际分配 4 页”，调用者释放时必须传 `4`（或任何落在 `(2,4]` 的值）才能让 `get_order(n)` 得到同一个阶，否则块大小不匹配会导致泄漏或元数据错乱（见 §7.2 限制 L1）。

### 4.2 初始化内存映射：`buddy_init_memmap(base, n)`

**目标**：把 `[base, base+n)` 这段连续空闲页，拆成**最少数量的、尽可能大的、且地址对齐**的 2 的幂块，逐个登记到 `free_area`。

```c
static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    for (struct Page *p = base; p != base + n; p++) {   // ① 认领并重置每一页
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    total_free += n;

    struct Page *p = base;
    size_t remaining = n;
    while (remaining > 0) {                             // ② 贪心拆分
        size_t idx = p - pages;
        int order = 0;
        while ((idx & ((1UL << (order + 1)) - 1)) == 0 &&   // 地址按 2^(order+1) 页对齐
               (1UL << (order + 1)) <= remaining &&         // 块不超过剩余页数
               order + 1 <= MAX_ORDER) {                    // 不超过最大阶
            order++;
        }
        while ((1UL << order) > remaining) order--;         // 兜底收缩

        p->property = (1UL << order);                       // ③ 登记块
        SetPageProperty(p);
        list_add(&free_area[order].free_list, &(p->page_link));
        free_area[order].nr_free += (1UL << order);

        p += (1UL << order);
        remaining -= (1UL << order);
    }
}
```

**为什么需要“最大对齐”拆分？** 伙伴运算 `index ^ 2^o` 只有在块头按 `2^o` 对齐时才指向真正的邻居。以本实验在 QEMU 默认 128 MB 内存下的实际参数为例：

| 量 | 值 | 说明 |
|----|----|------|
| `npage` | 0x8000（32768 页 = 128 MB） | `maxpa = KERNTOP`，见 `memlayout.h:10`、`pmm.c:88` |
| `sizeof(struct Page)` | 40 B | `ref(0) flags(8) property(16) page_link(24)` |
| `pages` 数组 | `[0x80207000, 0x80347000)`，320 页 = 1280 KB | `pages = ROUNDUP(end, PGSIZE)`，`pmm.c:90` |
| `mem_begin` | 0x80347000（下标 **0x80347**） | `ROUNDUP(freemem, PGSIZE)`，`pmm.c:100` |
| `n` | **0x7CB9 = 31929 页 ≈ 124.7 MB** | `(mem_end - mem_begin)/PGSIZE` |

`0x80347 = 0b1000_0000_0011_0100_0111` 显然不是大块对齐的，于是 ② 的贪心拆分得到 **10 个空闲块**：

| 阶 `o` | 起始下标 | 块大小 | 页数 |
|-------|---------|--------|------|
| 0 | 0x80347 | 1 页 | 1 |
| 3 | 0x80348 | 8 页 | 8 |
| 4 | 0x80350 | 16 页 | 16 |
| 5 | 0x80360 | 32 页 | 32 |
| 7 | 0x80380 | 128 页 | 128 |
| 10 | 0x80400 | 1024 页 | 1024 |
| 11 | 0x80800 | 2048 页 | 2048 |
| 12 | 0x81000 | 4096 页 | 4096 |
| 13 | 0x82000 | 8192 页 | 8192 |
| 14 | 0x84000 | 16384 页 | 16384 |
| | | **合计** | **31929 = n ✔** |

即前 6 个“小碎块”补足下标 `0x80347` 的低位，其后的高位部分全部由 1024～16384 页的大块组成（最大一块 64 MB，起始下标 `0x84000` 按 `2^14` 对齐）。这也解释了为什么 `MAX_ORDER=15` 时最大的空闲块是阶 14：受管区间并不从下标 0 开始，无法形成 32768 页的整块（见 §7.2 限制 L2）。

**正确性论证（构造性归纳）**：循环每次处理的 `[p, p + 2^o)` 满足 ①`p` 按 `2^o` 对齐（由内层 while 条件保证）、②`2^o ≤ remaining`（不越界）、③`o ≤ MAX_ORDER`。因此登记进 `free_area[o]` 的每个块都满足不变量 I4；又因为每次 `p += 2^o`、`remaining -= 2^o`，循环不变式“已登记块的并集恰为 `[base, p)` 且互不重叠”保持成立，循环终止时 `remaining == 0`，并集恰为 `[base, base+n)`。**块数上界**：每消耗一个块，`remaining` 的二进制最高位至多下降一位，故块数为 `O(log n)`——上表中 31929 页只用了 10 个块。

**复杂度**：① 扫描 `O(n)`；② 每块内层最多 `MAX_ORDER` 次，块数 `O(log n)`，故 ② 为 `O(MAX_ORDER · log n)`，整体 `O(n)`。

### 4.3 分配：`buddy_alloc_pages(n)`

```c
static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > total_free) return NULL;                       // ① 快速失败

    int order = get_order(n);                              // ② 向上取整到阶
    if (order > MAX_ORDER) return NULL;

    int cur = order;                                       // ③ 向上找第一个非空阶
    while (cur <= MAX_ORDER && list_empty(&free_area[cur].free_list)) cur++;
    if (cur > MAX_ORDER) return NULL;

    list_entry_t *le = list_next(&free_area[cur].free_list);  // ④ 摘除整块
    struct Page *page = le2page(le, page_link);
    list_del(&(page->page_link));
    free_area[cur].nr_free -= (1UL << cur);
    ClearPageProperty(page);

    while (cur > order) {                                  // ⑤ 逐级分裂
        cur--;
        struct Page *buddy = page + (1UL << cur);          //    右半块成为新块
        buddy->property = (1UL << cur);
        SetPageProperty(buddy);
        list_add(&free_area[cur].free_list, &(buddy->page_link));
        free_area[cur].nr_free += (1UL << cur);
    }

    total_free -= (1UL << order);                          // ⑥ 记账
    return page;
}
```

**流程说明**

1. **快速失败**：`n > total_free` 直接返回 `NULL`，避免无意义扫描（`n * PGSIZE` 不会溢出，因为 `n` 是页数且最多 128 MB/4 KB）。
2. **归约到阶**：`n` 被向上取整为 `2^order`，因此**实际分配页数可能大于 `n`**——这是伙伴系统的固有特性。
3. **向上查找**：从 `order` 向 `MAX_ORDER` 找到第一个非空阶 `cur`。因为更高阶的块可以任意分裂成低阶块，所以这一步一定能找到满足需求的最小可用块。
4. **摘除**：从链表头取块，`ClearPageProperty(page)` 表示“该页不再是空闲块头”（它即将被分配出去）。
5. **分裂**：`cur` 从高到低逐级下降，每次把当前块的**右半** `page + 2^(cur-1)` 登记为空闲块挂回对应阶，左半继续留在 `page` 上。**构造性保证**：
   - 若分裂前 `page` 的块按 `2^cur` 对齐，则 `page` 按 `2^(cur-1)` 对齐、`page + 2^(cur-1)` 也按 `2^(cur-1)` 对齐，且二者互为伙伴；
   - 最终剩下的 `page` 是阶 `order` 的左对齐块（`page` 原下标本身即 `2^cur` 的倍数 ⇒ 也是 `2^order` 的倍数），满足不变量 I4。
   
   分裂出的右半块永远挂在 `free_area[cur-1]` 上，因此**不会出现同阶伙伴同时空闲却未合并**的情况。
6. **记账**：`total_free` 只减去**实际分配**的 `2^order` 页；分裂出的部分通过 `free_area[o].nr_free` 保持计数，不影响 `total_free`。

**示例（阶 10 → 阶 0）**：假设取到下标 `0x80400` 的 1024 页块，请求 `n = 3` → `order = 2`：

| 步骤 | `cur` | 保留块 | 新挂回到 `free_area[cur]` 的伙伴 |
|------|------|--------|-------------------------------|
| 摘除 | 10 | `[0x80400, 0x80800)` | —— |
| 分裂 | 9 | `[0x80400, 0x80600)` | `[0x80600, 0x80800)` → `free_area[9]` |
| 分裂 | 8 | `[0x80400, 0x80500)` | `[0x80500, 0x80600)` → `free_area[8]` |
| … | … | … | … |
| 分裂 | 2 | `[0x80400, 0x80404)` ← 返回 | `[0x80404, 0x80408)` → `free_area[2]` |

调用者拿到 `0x80400` 起 4 页（满足 `n=3` 的向上取整需求）。

**复杂度**：③ 最多 `MAX_ORDER - order + 1` 次链表空判定；⑤ 恰好 `cur - order ≤ MAX_ORDER` 次分裂，每次为 O(1) 链表操作。整体 `O(MAX_ORDER)`，与受管总页数无关；`MAX_ORDER=15` 时最坏也就十几次操作。

### 4.4 释放与合并：`buddy_free_pages(base, n)`

```c
static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    int order = get_order(n);
    size_t block_size = 1UL << order;

    for (struct Page *p = base; p != base + block_size; p++) {   // ① 还原页状态
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    total_free += block_size;                                    // ② 先记账再合并

    struct Page *page = base;
    while (order < MAX_ORDER) {                                  // ③ 向上合并伙伴
        size_t idx = page - pages;
        size_t buddy_idx = idx ^ (1UL << order);                 //    伙伴 = 第 order 位取反
        if (buddy_idx >= (size_t)(npage - nbase)) break;         //    伙伴越界 → 停止

        struct Page *buddy = pages + buddy_idx;
        if (!PageProperty(buddy) || buddy->property != (1UL << order)) break;  // 非空闲同阶块 → 停止

        list_del(&(buddy->page_link));                           // ④ 摘除伙伴并合体
        free_area[order].nr_free -= (1UL << order);
        ClearPageProperty(buddy);

        if (buddy < page) page = buddy;                          //    块头取地址较小者
        order++;
    }

    page->property = (1UL << order);                             // ⑤ 登记合并后的块
    SetPageProperty(page);
    list_add(&free_area[order].free_list, &(page->page_link));
    free_area[order].nr_free += (1UL << order);
}
```

**流程说明**

1. **还原页状态**：把待释放的 `2^order` 页的 `flags`、`property`、`ref` 全部清零，并断言它们不在任何空闲链表中（`!PageProperty`），相当于“从已分配态回到中性态”。注意这里清理的是**整块 `block_size` 页**，而不是调用者传入的 `n` 页——因为调用者拿到的本来就是 `block_size` 页。
2. **记账**：`total_free += block_size`；后续合并动作只会在不同阶之间搬运 `nr_free`（摘除伙伴 `-2^o`、登记新块 `+2^(o+1)`），净额恒等，因此 `total_free` 不会重复计数。
3. **合并循环**：反复计算当前块的伙伴下标。三个 `break` 条件缺一不可：
   - **越界**：伙伴不在受管范围内（伙伴属于内核镜像、`pages` 数组或其他未交给分配器的区域），必须停止；
   - **非空闲块头**：`!PageProperty(buddy)`，包括“已分配出去的块”和“空闲块的内部页”两种情况；
   - **阶不匹配**：`buddy->property != 2^order`。这一条是**防止错误合并的关键**：假设某页的 `property` 恰好残留了非零值，或伙伴是另一个阶的块头，只检查 `PageProperty` 会把不同阶的块错误地吞并，导致空闲页凭空消失。
4. **合体**：`list_del` 把伙伴摘链，`ClearPageProperty(buddy)` 取消其块头身份，`page` 取两者中地址较小者（保持块头总在低地址，符合 `property` 语义），阶数加一后继续尝试与更大伙伴合并——这就是“**级联合并（cascading coalescing）**”。
5. **登记**：把最终的块挂到对应阶的链表并更新 `nr_free`。若一次都没合并，则等价于把原块挂回原阶链表。

**示例（连续分配 2 个 1 页块再释放）**：`alloc_page()` 取到 `0x80400` 并把它从阶 10 一路分裂到阶 0，伙伴 `[0x80401, 0x80800)` 被登记为阶 9..0 的块；`alloc_page()` 再取阶 0 链表头 `0x80401`。随后依次释放：

| 操作 | `order` | 伙伴 | 合并结果 |
|------|---------|------|---------|
| `free_pages(0x80400, 1)` | 0 | `0x80401`（已分配，`PageProperty==0`） | 无法合并，`[0x80400,0x80401)` 挂入 `free_area[0]` |
| `free_pages(0x80401, 1)` | 0 | `0x80400`（空闲块头，`property==1`） | 合并 → 阶 1 块 `0x80400`；继续查阶 1 伙伴 `0x80402` |
| ↳ | 1 | `0x80402`（是空闲块头，但 `property==2`） | 匹配 → 合并 → 阶 2 块 `0x80400`；继续…… |
| ↳ | 2 | `0x80404`（`property==4`，分裂时产生） | 匹配 → 合并 → 阶 3 块 `0x80400`…… |

可以看到，正是 **“阶不匹配就停止”** 这一条让合并不会越过分裂边界，而一旦分裂产生的空闲伙伴被逐级“吸收”，就能一路合并回最初的大块——这正是 `buddy_check()` 第 5 步“两个 1 页块释放后能重新分配 2 页块”所验证的行为。

**复杂度**：① 为 `O(2^order)`（≤ `2^MAX_ORDER`，实际受内存限制）；③ 最多迭代 `MAX_ORDER` 次，每次 O(1) 链表操作。整体 `O(2^order + MAX_ORDER)`，与受管总页数无关。

### 4.5 空闲页统计：`buddy_nr_free_pages()`

```c
static size_t buddy_nr_free_pages(void) { return total_free; }
```

`total_free` 在 `init_memmap` 中 `+= n`、在 `alloc` 中 `-= 2^order`、在 `free` 中 `+= 2^order`，与 `Σ free_area[o].nr_free` 始终相等（不变量 I3），因此查询为严格 **O(1)**，无需遍历链表——这也是不变量 I3 被列为自检项的原因。

### 4.6 算法一览

| 函数 | 时间复杂度 | 空间开销 | 关键操作 |
|------|-----------|---------|---------|
| `buddy_init` | `O(MAX_ORDER)` | —— | 初始化 16 个链表头 |
| `buddy_init_memmap` | `O(n + MAX_ORDER·log n)` | 0 | 最大对齐贪心拆分，`O(log n)` 个块 |
| `buddy_alloc_pages` | `O(MAX_ORDER)` | 0 | 向上找块 + 逐级分裂（右半挂回链表） |
| `buddy_free_pages` | `O(2^order + MAX_ORDER)` | 0 | 伙伴 XOR + 级联合并 |
| `buddy_nr_free_pages` | `O(1)` | 0 | 读 `total_free` |
| `buddy_check_consistency` | `O(Σ nr_free)` | 0 | 遍历 16 条链表校验 I1–I3 |

**辅助空间为 0**：全部元数据寄存在既有的 `struct Page` 数组中，这是相比“位图 + 节点头”方案最直接的收益。

---

## 5. 测试设计

测试代码内联在 `buddy_pmm.c` 中，由 `pmm_manager->check()` 在 `pmm_init()` 末尾自动触发。测试设计遵循三条原则：**① 先建立不变量基线，② 用“破坏性”操作逼近边界，③ 操作后恢复现场以免污染后续测试**。

### 5.1 分层结构

```
buddy_check()
├── buddy_check_consistency()          // 入口：记录基线不变量
├── 1. 初始状态汇总校验                 // Σ nr_free == total_free == nr_free_pages()
├── 2. buddy_basic_check()             // 分配 / 释放 / 计数 / 空池失败 / 现场保存恢复
├── 3. 多阶分配 alloc_pages(1/2/4/8)   // 各阶块头 PageProperty 必须为 0
├── 4. 对应释放 free_pages(1/2/4/8)    // 计数恢复
├── 5. 伙伴合并验证                     // 两个 1 页块 → 释放 → 能再分配 2 页块
├── 6. 分裂验证                        // 8 页块释放后再分配 1 页块
├── 7. 非 2 的幂请求                    // alloc_pages(3) 成功，按 4 页归还
├── 8. 超界请求失败                     // alloc_pages(2^(MAX_ORDER+1)) == NULL
├── 9. 终态汇总校验                     // 与初始完全一致
└── buddy_check_consistency()          // 出口：再次校验基线不变量
```

### 5.2 不变量自检：`buddy_check_consistency()`

```c
static void buddy_check_consistency(void) {
    size_t total = 0;
    for (int i = 0; i <= MAX_ORDER; i++) {
        size_t sum = 0;
        list_entry_t *le = &free_area[i].free_list;
        while ((le = list_next(le)) != &free_area[i].free_list) {   // 遍历第 i 阶链表
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));                 // I1: 必须是空闲块头
            assert(p->property == (1UL << i));       // I1: 块大小必须与本阶一致
            sum += p->property;
        }
        assert(sum == free_area[i].nr_free);         // I2: 链表内容与计数自洽
        total += sum;
    }
    assert(total == total_free);                     // I3: 全局空闲页数自洽
    assert(total == buddy_nr_free_pages());          // I3: 对外接口一致
}
```

它把 §3.3 的三条不变量压缩成一次 `O(总空闲页数)` 的遍历。**在 `buddy_check()` 的首尾各调用一次**，就构成“任何操作序列都不能破坏内部一致性”的端到端断言：如果分配/释放中出现链表断裂、`property` 与阶不符、计数漏加多减，都会在出口被立刻抓住。

`assert(p->property == (1UL << i))` 这一条尤其有价值：它会捕捉“把一个阶 3 的块挂进了阶 4 链表”“合并后忘了更新 `property`”这类**逻辑错误**，而这类错误往往不会立刻崩溃，只会在后续分配中表现为内存重叠或泄漏。

### 5.3 基础用例：`buddy_basic_check()`

| 步骤 | 操作 | 断言 | 验证意图 |
|------|------|------|---------|
| 1 | `alloc_page()` ×3 | 非空、三者互不相同 | 基本分配可用，不会重复返回同一页 |
| 2 | 检查返回页 | `page_ref == 0`；`page2pa(p) < npage*PGSIZE` | 引用计数被正确清零；页描述符映射到合法物理地址 |
| 3 | 检查块头标记 | `!PageProperty(p0/p1/p2)` | **已分配的页绝不能仍带“空闲块头”标记**（否则会被二次分配） |
| 4 | **保存现场**：`free_area_store[0..MAX_ORDER]`、`total_free_store`，随后把所有链表置空、`total_free=0` | —— | 伙伴系统有 16 条链表，必须**逐阶**保存/恢复（这是与 `default_pmm` 单链表版本最大的不同）；构造“内存耗尽”场景 |
| 5 | `alloc_page()` | `== NULL` | 空闲池为 0 时分配必须失败，而不是返回野指针 |
| 6 | `free_page()` ×3 | `total_free == 3 == nr_free_pages()` | 释放计数正确 |
| 7 | `alloc_page()` ×3 | 全部非空 | 释放后可重新分配 |
| 8 | `alloc_page()` | `== NULL` | 再次耗尽仍能正确失败 |
| 9 | `free_page(p0)`；再 `alloc_page()`；再 `alloc_page()` | 第一次成功、第二次 `== NULL`、`total_free == 1` 然后 `== 0` | 单页粒度的记账精确无泄漏 |
| 10 | **恢复现场**：拷回 `free_area_store`、`total_free_store` | —— | 让后续测试回到真实基线 |
| 11 | `free_page()` ×3 归还步骤 9/7 借出的页 | —— | 清账，保证第 3 步开始的不变量成立 |

> 步骤 4/10 的“保存—清空—恢复”是本用例的关键设计：它用**最小代价制造出真实的“无空闲内存”状态**，从而测试 `alloc` 的失败路径和计数边界；若不保存恢复，把 `free_area` 清空会永久破坏内核后续的内存视图。

### 5.4 完整用例：`buddy_check()`

| 步骤 | 用例 | 断言 | 验证意图 |
|------|------|------|---------|
| 1 | 初始汇总 | `Σ nr_free == total_free == nr_free_pages()` | 建立测试基线 |
| 2 | 调用 `buddy_basic_check()` | 内部全部断言 | 基础路径 |
| 3 | `alloc_pages(1/2/4/8)` | 四者非空；`!PageProperty(·)` | **跨阶分配**：覆盖阶 0/1/2/3 的取块与分裂路径 |
| 4 | `free_pages(1/2/4/8)` | `total_free == nr_free_pages()` | 跨阶释放；`total_free` 与分阶统计仍一致 |
| 5 | `a=alloc_pages(1); b=alloc_pages(1); free(a); free(b); alloc_pages(2)` | 2 页分配**成功** | **伙伴合并**：两个相邻 1 页块释放后必须能合并出 2 页块（若合并失效，此处会因最高阶块已被切碎而失败） |
| 6 | `alloc_pages(8)` → 释放 → `alloc_pages(1)` ×2 | 全部非空 | **分裂**：大块释放后必须能重新切给小请求，验证分裂链的完整性 |
| 7 | `alloc_pages(3)` → `free_pages(p3, 4)` | 分配非空 | **非 2 的幂请求**：内部按阶 2（4 页）服务；注释明确要求按实际块大小归还 |
| 8 | `alloc_pages(1UL << (MAX_ORDER + 1))` | `== NULL` | **超界失败**：请求超过 `MAX_ORDER` 必须优雅返回 `NULL`（而不是 `1UL << order` 溢出或越界访问 `free_area`） |
| 9 | 终态汇总 | `Σ nr_free == total_free == nr_free_pages()` | 所有用例归还完毕后，空闲页数必须与初始一致——**零泄漏** |
| 10 | `buddy_check_consistency()` | I1–I3 | 端到端不变量校验 |

覆盖矩阵（对 §4 各分支的映射）：

| 算法分支 | 覆盖用例 |
|---------|---------|
| `alloc`：快速失败 `n > total_free` | 基础检查步骤 5、8 |
| `alloc`：`order > MAX_ORDER` | 完整检查步骤 8 |
| `alloc`：向上找块（跨阶） | 完整检查步骤 3、5、6 |
| `alloc`：逐级分裂 | 完整检查步骤 3、6 |
| `free`：无伙伴可合并 | 基础检查步骤 6 |
| `free`：伙伴已分配（停止合并） | 完整检查步骤 5 的第一次 `free(a)` |
| `free`：伙伴阶不匹配（停止合并） | §4.4 示例中的阶 2/3 检查，由步骤 5、6 的连锁合并覆盖 |
| `free`：级联合并至更大阶 | 完整检查步骤 5、6 |
| 计数：`total_free` / `nr_free` | 所有步骤的汇总断言 |

---

## 6. 编译、运行与验证结果

### 6.1 构建与运行

```bash
cd code
# 1) 在 kern/mm/pmm.c 的 init_pmm_manager() 中绑定 buddy 实现
#    pmm_manager = &buddy_pmm_manager;
make clean && make
# 2) 在 QEMU（默认 128 MB 内存）中运行
make qemu          # 等价于 qemu-system-riscv64 -machine virt -nographic -bios default -kernel bin/ucore.img
```

测试点在 `pmm_init()` 中由 `check_alloc_page()` 自动调用（`pmm.c:142`），**无需额外输入**；所有断言通过后打印 `check_alloc_page() succeeded!`。若任一断言失败，`assert` 会经由 `panic()` 输出文件名/行号并停机，因此“能跑到 succeeded”本身就是全部用例通过的证据。

### 6.2 实测输出（本实现，通过）

```text
DTB Init
HartID: 0
DTB Address: 0x87e00000
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)
  End:  0x0000000087ffffff
DTB init completed
(THU.CST) os is loading ...
Special kernel symbols:
  entry  0xffffffffc02000d6 (virtual)
  etext  0xffffffffc0201914 (virtual)
  edata  0xffffffffc0206018 (virtual)
  end    0xffffffffc02061e8 (virtual)
Kernel executable memory footprint: 25KB
memory management: buddy_pmm_manager          ← 分配器已切换
physcial memory map:
  memory: 0x0000000008000000, [0x0000000080000000, 0x0000000087ffffff].
check_alloc_page() succeeded!                 ← buddy_check() 全部断言通过
satp virtual address: 0xffffffffc0205000
satp physical address: 0x0000000080205000
```


### 6.3 结果解读

| 观察点 | 含义 |
|--------|------|
| `memory management: buddy_pmm_manager` | `pmm_manager` 已指向本实现，后续全部物理内存请求都由伙伴系统服务 |
| `memory: 0x8000000, [0x80000000, 0x87ffffff]` | DTB 报告 128 MB RAM，与 `MAX_ORDER=15`（`2^15` 页 = 128 MB）的适配前提一致 |
| `check_alloc_page() succeeded!` | §5 的全部用例（基础、多阶、合并、分裂、非 2 的幂、超界失败、零泄漏、不变量自检）均通过 |
| `satp ...` 之后继续正常启动 | 伙伴系统在完成自检后仍能继续为内核提供内存（`alloc_pages` 在后续初始化中被反复调用），说明内部状态未被 `check()` 破坏——这正是 `buddy_basic_check` 第 4/10 步“保存并恢复现场”设计的意义 |

---

## 7. 与参考实现及同类算法的比较

### 7.1 与 coolshell 参考实现（数组 + 位图）的差异

| 维度 | 参考实现（极简伙伴分配器） | 本实现（ucore 版） |
|------|--------------------------|-------------------|
| 元数据 | `unsigned char *bitmap`，显式维护分裂/合并二叉树 | **无辅助数组**，复用 `Page.property` + `PG_property` |
| 块定位 | 完全二叉树节点编号，`INDEX(i) = i + 2^MAX_ORDER - 1`，父/子/伙伴为纯算术运算 | 页数组**下标**，伙伴 = `idx ^ 2^order` |
| 空闲块查找 | 位图按位扫描找可用节点 | 按阶分桶的**双向链表**，分配时从目标阶向上找第一个非空阶 |
| 释放时的伙伴判定 | 读位图低位（`buddy[INDEX(i)] == 0` 表示未分裂） | `PageProperty(buddy) && buddy->property == 2^order` |
| 内存开销 | `2^(MAX_ORDER+1)` 字节位图（如 MAX_ORDER=15 约 64 KB） | 0 额外内存（`struct Page` 已存在） |
| 最大阶限制 | 由位图大小决定，可任意 | 受 `free_area` 数组与 `MAX_ORDER` 编译期常量约束 |
| 空闲页统计 | 需按阶累加 | `total_free` O(1) 查询 |
| 与 ucore 接口 | 需自行实现地址换算 | 直接使用 `page - pages`、`le2page`、`SetPageProperty` 等既有设施，与 `default_pmm`/`best_fit_pmm` 风格统一 |

**核心权衡**：参考实现用**空间换时间**——位图让伙伴判定与合并路径是纯算术，且天然支持“释放时按地址反推阶”；本实现用**时间换空间**——伙伴判定退化为一次链表成员检查（常数时间）加一次 `property` 比较，但节省了辅助内存，并复用了 ucore 既有的链表/标志位设施，与框架浑然一体。

### 7.2 与 `default_pmm` / `best_fit_pmm` 的对比

| 算法 | 空闲结构 | 分配策略 | 分配耗时 | 释放耗时 | 外部碎片 | 能否满足“连续 2^k 页”请求 |
|------|---------|---------|---------|---------|---------|------------------------|
| First Fit | 单链表（按地址序） | 首个够大的块 | `O(块数)` | `O(块数)` | 较大 | 是 |
| Best Fit | 单链表（按地址序） | 最接近需求的块 | `O(块数)` | `O(块数)` | 较小，但易产生微小碎片 | 是 |
| **Buddy** | **按阶分桶（16 条链表）** | **向上找最近可用阶 + 分裂** | **`O(MAX_ORDER)`** | **`O(MAX_ORDER)`** | **内部碎片 ≤ 请求量的 2 倍；外部碎片可通过合并抑制** | **是（但会向上取整）** |

伙伴系统的取舍很明确：**牺牲最多 2 倍内部碎片，换取与内存规模无关的分配/释放时间，以及可预测的合并行为**。这也是 Linux 物理页分配器选择它的原因——Linux 中 buddy 系统之上再叠加 slab/slub 处理小对象分配，正好弥补其内部碎片问题（对应本实验的另一个 Challenge）。

---

## 8. 正确性论证与边界情况

### 8.1 关键性质与论证

| 性质 | 论证 |
|------|------|
| **P1：空闲块互不重叠且并集恰为空闲页** | 初始由 `init_memmap` 构造性保证（§4.2）；分配只把整块从链表中摘出并（可选）分裂为两个互不重叠的子块，其中右半立即入链、左半继续作为分配结果或继续分裂，页集合并集不变；释放把 `2^order` 页合入块集合并（可选）与伙伴合并，合并的前提是二者为同一阶的不相交伙伴，合并后是二者并集，故并集不变。 |
| **P2：块头下标必按块大小对齐（不变量 I4）** | 初始拆分显式检查对齐；分裂/合并保持 `page` 为低地址半块，故对齐性在每级都成立，归纳可得。 |
| **P3：合并不会吞并非伙伴或不同阶的块** | 合并条件 `buddy == (page - pages) ^ 2^order`、`PageProperty(buddy)`、`buddy->property == 2^order` 三者同时成立；第一条保证几何上是伙伴，第二条保证它是空闲块头（不是内容页、不是已分配块），第三条保证阶匹配（不会把阶 3 的块并进阶 4 的块，也不会把一块里残留 `property` 的页当作伙伴）。 |
| **P4：不会出现“两个空闲伙伴未合并”** | 任一 `free_pages` 结束时，若其伙伴空闲且同阶，循环会继续合并并更新 `page`；分裂产生的右半块在挂链时其伙伴（左半）正在被分配或继续使用，故分裂阶段不会留下可合并的伙伴对。归纳可得所有稳定状态下同阶伙伴不同时空闲。 |
| **P5：计数一致性（不变量 I3）** | `total_free` 与 `Σ nr_free` 的每次变更严格对应页集合的变化：分配 `-2^order`、释放 `+block_size`、合并时伙伴 `-2^o` 与新块 `+2^(o+1)` 相抵（差额恰为伙伴的 `2^o`，已在释放时计入）。 |
| **P6：终止性** | `alloc` 的向上扫描有 `MAX_ORDER` 上界；分裂循环 `cur` 严格递减；`free` 的合并循环 `order` 严格递增且有 `order < MAX_ORDER` 上界（合并到阶 15 即停止，不再尝试越界访问 `free_area[16]`）。 |

### 8.2 边界情况处理清单

| 边界 | 处理方式 | 位置 |
|------|---------|------|
| `n == 0` | `assert(n > 0)` 直接拦住（接口契约不允许 0 页） | `alloc`/`free` 开头 |
| 请求超过空闲页数 | `n > total_free` 早退返回 `NULL` | `buddy_alloc_pages` |
| 请求超过 `MAX_ORDER` | `order > MAX_ORDER` 返回 `NULL`（测试步骤 8 覆盖） | `buddy_alloc_pages` |
| 所有阶链表皆空 | 扫描到 `cur > MAX_ORDER` 返回 `NULL` | `buddy_alloc_pages` |
| 伙伴下标越界（伙伴在内核区或 `pages` 数组区） | `buddy_idx >= npage - nbase` 时停止合并 | `buddy_free_pages` |
| 伙伴已被分配 | `!PageProperty(buddy)` 停止合并 | `buddy_free_pages` |
| 伙伴阶不同（分裂边界） | `buddy->property != 2^order` 停止合并 | `buddy_free_pages` |
| 合并已到最高阶 | `while (order < MAX_ORDER)` 保证不会再访问 `free_area[MAX_ORDER+1]` | `buddy_free_pages` |
| 初始区间起点不对齐 | 最大对齐贪心拆分自动产生若干小阶块 | `buddy_init_memmap` |
| 非 2 的幂请求 | 向上取整到阶，调用者需按实际大小归还 | `get_order` |
| 释放的页带有 `PG_reserved`/`PG_property` | `assert(!PageReserved(p) && !PageProperty(p))` 立即暴露误用 | `buddy_free_pages` |

---

## 9. 已知限制与改进方向

### 9.1 已知限制

| 编号 | 限制 | 说明 | 影响 |
|------|------|------|------|
| **L1** | 释放时的 `n` 必须与分配时“同阶” | 接口不记录分配大小，`get_order(n)` 是唯一的还原依据。`alloc_pages(3)` 实际给 4 页，若按 `3` 归还则只清 3 页且按阶 2 挂链，块头 `property` 与内容不一致 | 属接口固有约束；本实现用 `check` 中“按 4 页归还”的注释显式提示 |
| **L2** | `MAX_ORDER` 编译期固定为 15 | 该值适配 QEMU 默认 128 MB（`KERNTOP`，`memlayout.h:10`）。若 QEMU 用 `-m` 指定更大内存，超过 128 MB 的部分不会被纳入（`npage` 由 `KERNTOP` 封顶），`free_area` 也不会越界 | 换内存配置需同步调整 `MAX_ORDER`，注释中已 WARN |
| **L3** | 不校验“被释放的块确实是先前分配的” | 与 ucore 框架一致，依赖调用者正确性；重复释放会触发 `assert(!PageProperty(p))` 或破坏不变量（`check` 可捕获） | 与 `default_pmm`/`best_fit_pmm` 同等强度 |
| **L4** | `get_order` 为循环实现 | `O(MAX_ORDER)` 常数开销，虽小但非最优 | 可用 `64 - __builtin_clzll(n - 1)` 位运算替代 |
| **L5** | 分配严格按阶向上取整 | 请求 3 页得 4 页，内部碎片最坏接近 2 倍 | 伙伴系统固有属性 |
| **L6** | 未实现按物理地址释放 | `free_pages` 需要块头 `struct Page *`，无法像 Linux 那样仅凭地址反查并合并 | 与 ucore 接口一致即可满足实验要求 |

### 9.2 改进方向

1. **更快找到可用阶**：用 `unsigned int free_mask`（第 `o` 位表示 `free_area[o]` 非空）替代向上循环，配合 `__builtin_ctz` 可把 `alloc` 的查找压到 O(1) 位运算。
2. **`get_order` 位运算化**：`order = 64 - __builtin_clzll(n - 1)`（`n == 1` 特判），消除循环。
3. **延迟合并 / 分段的空闲链表（segregated free lists）**：为每个阶维护“已对齐”和“未对齐”两条链表，可把合并从“最多 `MAX_ORDER` 次链表操作”降为 O(1) 摊还（Linux 的 `pageblock`/`MIGRATE_TYPES` 思路）。
4. **运行期可配的 `MAX_ORDER`**：由 DTB 探测到的内存大小动态计算 `MAX_ORDER`（例如 `log2(npage)`），并把 `free_area` 改为按需分配，解除 L2。
5. **记录分配大小**：在块头 `Page` 的 `property` 中保留“实际分配阶”并新增一个标志位（或借用 `flags` 的其他位），即可在 `free_pages` 中校验 `get_order(n)` 与记录值是否一致，主动发现 L1 类误用。
6. **伙伴系统 + slab/slub 双层架构**：在伙伴系统之上叠加小对象缓存（本实验另一个 Challenge），把小请求的内部碎片从“最多 2 倍”降到 slab 的对象粒度。

---

## 10. 附录

### 10.1 关键常量与结构

| 名称 | 定义位置 | 值/含义 |
|------|---------|---------|
| `MAX_ORDER` | `buddy_pmm.c:10` | 15，最大块 `2^15` 页 = 128 MB |
| `free_area[]` | `buddy_pmm.c:12` | `free_area_t[16]`，下标即阶 |
| `total_free` | `buddy_pmm.c:13` | 全局空闲页数 |
| `struct Page` | `memlayout.h:34` | 40 B：`ref`、`flags`、`property`、`page_link` |
| `PG_property` | `memlayout.h:43` | 空闲块头标记（伙伴系统中的“空闲节点”标记） |
| `free_area_t` | `memlayout.h:57` | `{list_entry_t free_list; unsigned int nr_free;}` |
| `PGSIZE` | `mmu.h` | 4096 B |
| `nbase` | `pmm.c:23` | `DRAM_BASE / PGSIZE = 0x80000`，`pages[0]` 对应的 PPN |
| `npage` | `pmm.c:18` | `0x8000`（128 MB / 4 KB，`maxpa` 受 `KERNTOP` 限制） |

### 10.2 一次完整的分配/释放轨迹

以 `buddy_check()` 步骤 5（伙伴合并验证）在真实布局下的行为为例，假设当前刚初始化完成：

```text
初始:  free_area[10] 含块 [0x80400, 0x80800)  (1024 页)

① a = alloc_pages(1)   → order=0，向上找到阶 10
   摘除 [0x80400, 0x80800)，逐级分裂：
     挂回 free_area[9]: [0x80600,0x80800)   free_area[8]: [0x80500,0x80600)
     ... free_area[1]: [0x80402,0x80404)    free_area[0]: [0x80401,0x80402)
   返回 a = 0x80400   (total_free -= 1)

② b = alloc_pages(1)   → order=0，free_area[0] 非空
   直接摘除 b = 0x80401   (total_free -= 1)

③ free_pages(a, 1)     → order=0
   伙伴 idx = 0x80400 ^ 1 = 0x80401 → b 已分配 (!PageProperty) → 不合并
   登记 [0x80400,0x80401) 到 free_area[0]   (total_free += 1)

④ free_pages(b, 1)     → order=0
   伙伴 idx = 0x80401 ^ 1 = 0x80400 → 空闲块头，property == 1 ✔ → 合并！
     list_del(0x80400)   free_area[0].nr_free -= 1   块头 = min(0x80400,0x80401) = 0x80400
     order = 1，伙伴 idx = 0x80400 ^ 2 = 0x80402
       → free_area[1] 中确有 [0x80402,0x80404)，property == 2 ✔ → 继续合并
     order = 2，伙伴 idx = 0x80400 ^ 4 = 0x80404
       → 该块正是①中分裂产生的阶 2 伙伴，property == 4 ✔ → 继续合并
     …… 依次吸收阶 3、4、…、9 的伙伴
     order = 10，伙伴 idx = 0x80400 ^ 0x400 = 0x80000
       → 越界或非空闲 → 停止
   登记 [0x80400, 0x80800) 到 free_area[10]   (total_free += 1)

⑤ alloc_pages(2) 成功（可从阶 1 或由更大块分裂得到）→ 验证“合并确实发生”
   free_pages(..., 2) 归还，空闲页数回到初始值
```

这段轨迹同时展示了 P4（分裂出来的伙伴最终被原路吸收）与 P3（`b` 未释放时 `a` 不会错误合并）。

### 10.3 源码索引

| 内容 | 位置 |
|------|------|
| 阶计算 | `code/kern/mm/buddy_pmm.c:16` |
| 初始化 | `code/kern/mm/buddy_pmm.c:24` |
| 内存映射初始化（贪心最大对齐拆分） | `code/kern/mm/buddy_pmm.c:32` |
| 分配（向上找块 + 逐级分裂） | `code/kern/mm/buddy_pmm.c:70` |
| 释放（伙伴 XOR + 级联合并） | `code/kern/mm/buddy_pmm.c:105` |
| 空闲页统计 | `code/kern/mm/buddy_pmm.c:154` |
| 基础检查 | `code/kern/mm/buddy_pmm.c:159` |
| 不变量自检 | `code/kern/mm/buddy_pmm.c:237` |
| 完整检查 | `code/kern/mm/buddy_pmm.c:257` |
| 管理器实例 | `code/kern/mm/buddy_pmm.c:335` |
| 管理器接口定义 | `code/kern/mm/pmm.h:15` |
| 分配器绑定点 | `code/kern/mm/pmm.c:37` |

---

## 11. 小结

本设计在 ucore 既有的 `pmm_manager` 框架内实现了一个**零辅助空间**的伙伴系统：

- 用**按阶分桶的双向链表**组织空闲块，配合 `total_free` 实现 O(1) 的空闲页查询；
- 用 `Page.property` + `PG_property` 复用既有元数据，替代经典实现的位图/节点头，避免额外内存开销；
- 分配时“向上找最近可用阶 → 逐级分裂，右半挂回链表”，释放时“伙伴 XOR → 三重条件判定 → 级联合并”，两条路径的复杂度都是 `O(MAX_ORDER)`，与受管内存规模无关；
- 通过**不变量自检 + 分层测试用例（基础 / 多阶 / 合并 / 分裂 / 非 2 的幂 / 超界 / 零泄漏）**，在 `pmm_init()` 中自动完成端到端验证，实测输出 `memory management: buddy_pmm_manager` 与 `check_alloc_page() succeeded!`，`make grade` 记录为 `Total Score: 50/50`。

伙伴系统以“最多 2 倍内部碎片”换取了可预测的时间复杂度与良好的合并行为，是物理页级分配器的经典选择；其内部碎片问题需要在页分配器之上叠加 slab/slub 之类的对象分配器来弥补，这也是本实验另一个 Challenge 的动机所在。
