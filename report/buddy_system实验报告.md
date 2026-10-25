# buddy system 实验报告

## [PROMPT]

**做什么**：在 `code/kern/mm/buddy_pmm.c` 中实现伙伴系统物理页管理器。把物理页按 2 的幂分成不同阶的空闲块，分配时向上找足够大的块并逐级二分，释放时与同阶伙伴合并，最终向 `struct pmm_manager` 提供初始化、建图、分配、释放、计数和自检接口。

**改哪里**：实现只落在 `code/kern/mm/buddy_pmm.c`，辅助函数全部设为 `static`；不修改 `pmm.h`、`memlayout.h`、`libs/list.h` 及其他分配器。启用时只需在 `code/kern/mm/pmm.c` 的 `init_pmm_manager()` 中把绑定改为 `&buddy_pmm_manager`，其余启动流程不变。

**怎么改**：

1. 定义 `MAX_ORDER 15`、`free_area[MAX_ORDER + 1]` 和 `total_free`。阶 `k` 的块大小为 `2^k` 页，`free_area[k]` 挂所有 `2^k` 页空闲块。
2. 空闲块的起始页称为块头：`property = 2^k`，置 `PG_property`，用 `page_link` 入链；块内其余页不置位。
3. `init_memmap` 按页下标对齐把连续区间拆成若干 2 的幂块；`alloc_pages` 从第一个足够大的非空阶取块，逐级二分后返回左半；`free_pages` 用异或找伙伴，只与同阶空闲块头合并。
4. 每次分配/释放同步维护 `free_area[k].nr_free` 和 `total_free`，两者单位都是“页”而不是“块”。
5. 增加 `clear_area`/`restore_area` 及场景检查，由 `buddy_check` 串联。

```text
分配：从 8 页块中取 1 页
8 ──► 4(保留) + 4(阶2)
4(保留) ──► 2(保留) + 2(阶1)
2(保留) ──► 1(返回) + 1(阶0)
释放：1 页先与同阶伙伴并为 2 页，再逐级向上合并
```

## [RELY]

**数据结构**

```c
struct Page {
    int ref;                  // 引用计数
    uint64_t flags;           // 页标志
    unsigned int property;    // 空闲块的页数，仅块头有效
    list_entry_t page_link;   // 空闲链表节点
};

typedef struct {
    list_entry_t free_list;   // 该阶空闲块链表头
    unsigned int nr_free;     // 该阶空闲页总数
} free_area_t;
```

**宏与标志**

- `PG_property`、`SetPageProperty`、`ClearPageProperty`、`PageProperty`：标记和判断空闲块头。
- `PG_reserved`、`PageReserved`：`init_memmap` 入口处确认传入页仍是保留页。
- `le2page(le, page_link)`：由链表节点取得对应的 `struct Page *`。
- `PGSIZE`：页大小；`pages`、`npage`、`nbase`：页描述符数组的基址、物理页总数和起始页号偏移。

**已有函数与全局量**

- `list_init`、`list_add`、`list_del`、`list_empty`、`list_next`：双向链表操作；`list_add(head, elm)` 插在头结点之后。
- `set_page_ref`、`page_ref`、`page2pa`：引用计数与地址转换，供分配和检查使用。
- `struct pmm_manager` 及 `alloc_pages`/`free_pages`/`nr_free_pages` 包装；测试可直接使用 `alloc_page()`、`free_page()` 宏。
- `assert`：校验前置条件、页状态和计数一致性。
- 检查函数与分配器在同一文件，可直接读写 `free_area` 和 `total_free`，测试也依赖这两个名字做保存、清空和恢复。
- `buddy_pmm.h` 已提供 `extern const struct pmm_manager buddy_pmm_manager;`，定义处需与之一致。

## [GUARANTEE]

**核心交付**

```c
static void buddy_init(void);
static void buddy_init_memmap(struct Page *base, size_t n);
static struct Page *buddy_alloc_pages(size_t n);
static void buddy_free_pages(struct Page *base, size_t n);
static size_t buddy_nr_free_pages(void);
```

另外定义并初始化 `const struct pmm_manager buddy_pmm_manager`，成员绑定见 §10。

**辅助与自检**

```c
static int get_order(size_t n);
static unsigned int clear_area(free_area_t store[]);
static void restore_area(free_area_t store[], unsigned int total_free_store);
static void buddy_basic_check(void);
static void buddy_check_consistency(void);
static void check_coalesce(void);
static void check_split(void);
static void check_non_power_of_two(void);
static void buddy_check(void);
```

**必须保持**

- 全局状态保留 `free_area[MAX_ORDER + 1]` 和 `unsigned int total_free`，名称和类型不改变。
- `buddy_pmm_manager` 的 `.name` 为 `"buddy_pmm_manager"`，五个管理函数和 `.check` 必须全部绑定。
- 不修改 `struct pmm_manager`、`struct Page`、`free_area_t` 的定义，不修改已有测试的语义和外部接口。
- 分配器只依赖本文件状态和上述公共设施，不引入新的全局依赖。

## [SPECIFICATION]

### 1. 求阶：`get_order(n)`

**前置**：`n > 0`。

**行为**：返回最小的非负整数 `k`，使 `2^k >= n`。例如 `1→0`、`2→1`、`3→2`、`4→2`、`5→3`。

**后置**：无副作用，不改变任何页或链表。

### 2. 初始化：`buddy_init()`

**前置**：分配器尚未使用，可以在 `init_memmap` 之前调用。

**行为**：对 `k = 0..MAX_ORDER` 调用 `list_init(&free_area[k].free_list)` 并把 `nr_free` 置 0；把 `total_free` 置 0。

**后置**：16 个阶的链表均为空，所有计数为 0；不触碰 `pages` 中的页，可重复调用。

### 3. 加入空闲页：`buddy_init_memmap(base, n)`

**前置**：`n > 0`；从 `base` 开始的 `n` 页连续、有效，当前均为保留页（`PageReserved`），且尚未加入空闲链表。

**行为**：

1. 逐页断言 `PageReserved(p)`，把 `flags` 和 `property` 清零，引用计数设为 0。这一步同时清除了保留位。
2. `total_free += n`。
3. 从 `p = base`、`remaining = n` 开始循环，直到 `remaining == 0`：
   - 令 `idx = p - pages`，取最大的 `k`（`0 <= k <= MAX_ORDER`），满足 `idx` 能被 `2^k` 整除且 `2^k <= remaining`；
   - 令 `p->property = 2^k`，置 `PG_property`，把 `p` 挂到 `free_area[k]`，并把 `free_area[k].nr_free` 增加 `2^k`；
   - `p += 2^k`，`remaining -= 2^k`。

**后置**：整个区间被登记为若干对齐的 2 的幂块；每块只有起始页带 `PG_property`，`property` 等于该块页数；本次 `n` 页全部进入链表，各阶 `nr_free` 的总和与 `total_free` 都增加 `n`（本实验首次调用时二者都等于 `n`）。

**说明**：对齐条件保证每个块的首地址按自身大小对齐，后续才能用异或找伙伴；块不会跨越传入区间的边界。

### 4. 分配页面：`buddy_alloc_pages(n)`

**前置**：`n > 0`。

**行为**：

1. 若 `n > total_free`，直接返回 `NULL`。
2. 令 `k = get_order(n)`；若 `k > MAX_ORDER`，返回 `NULL`。
3. 从 `cur = k` 开始向上扫描，找到第一个非空的 `free_area[cur]`；若直到 `MAX_ORDER` 都为空，返回 `NULL`。
4. 取该链表首节点对应的页 `page`，从链表删除，`free_area[cur].nr_free` 减去 `2^cur`，并清除 `page` 的 `PG_property`。
5. 当 `cur > k` 时逐级分裂：
   - `cur--`；
   - 令 `buddy = page + 2^cur`；
   - 设置 `buddy->property = 2^cur` 和 `PG_property`，把 `buddy` 挂到 `free_area[cur]`，并将该阶 `nr_free` 增加 `2^cur`。
6. `total_free` 减去 `2^k`，返回 `page`。

**后置**：返回值的起始下标按 `2^k` 对齐，连续 `2^k` 页可用；返回块头不带 `PG_property`，引用计数为 0；空闲页总数减少 `2^k`；各阶链表和计数保持一致。

**失败**：上述失败路径都在修改链表之前返回，不改变原有状态。

**说明**：

- 请求不是 2 的幂时向上取整，例如申请 3 页实际占用 4 页，多出的 1 页是内部碎片，不能再次分配。
- 同阶只取链表首节点，不保证地址顺序，也不保证最优适配。
- `ClearPageProperty()` 只清标志位，`property` 字段可能保留取块前的旧值；该字段仅在 `PG_property` 为 1 时有意义，不能据此判断已分配页的块大小。

### 5. 释放页面：`buddy_free_pages(base, n)`

**前置**：`n > 0`；`base` 是 `buddy_alloc_pages` 返回的块头；`2^get_order(n)` 等于该次分配的块大小；块内所有页仍处于已分配状态；`base` 按块大小对齐。调用方不会重复释放。

**行为**：

1. 令 `k = get_order(n)`，`size = 2^k`。
2. 对 `base` 到 `base + size - 1` 的每一页：断言它不是保留页且不带 `PG_property`；把 `flags`、`property` 清零，引用计数设为 0。
3. `total_free += size`。
4. 令 `page = base`，当 `k < MAX_ORDER` 时循环：
   - 令 `idx = page - pages`，`buddy_idx = idx ^ (1UL << k)`；
   - 若 `buddy_idx >= npage - nbase`，停止合并；
   - 令 `buddy = pages + buddy_idx`；若 `buddy` 不是空闲块头，或 `buddy->property != 2^k`，停止合并；
   - 从 `free_area[k]` 删除 `buddy`，该阶 `nr_free` 减去 `2^k`，清除 `buddy` 的 `PG_property`；
   - 若 `buddy` 的地址更小，令 `page = buddy`；
   - `k++`。
5. 把最终块登记回去：`page->property = 2^k`，置 `PG_property`，挂到 `free_area[k]`，该阶 `nr_free` 增加 `2^k`。

**后置**：`[base, base + size)` 恢复为空闲；合并只在伙伴为同阶空闲块时发生，合并后的块头是参与合并的页中地址最小者；`total_free` 增加 `size`；各阶计数与链表一致。

**说明**：

- 伙伴地址由当前块首下标异或块大小得到，只有伙伴也是同阶空闲块头时才合并；已分配块、更大块或更小的块都不会被误并。
- 伙伴下标越过 `npage - nbase` 时停止，防止合并到管理范围之外。
- 不支持部分释放：不能只归还一个已分配块的一部分；释放的页数是 `n` 向上取整后的 `2^k`，`n` 与分配块大小不匹配会导致状态错误。

### 6. 查询空闲页数：`buddy_nr_free_pages()`

**前置**：分配器已初始化。

**后置**：返回 `total_free`，不改变任何状态。返回值是空闲页数，不是空闲块数。

### 7. 测试辅助：`clear_area` 与 `restore_area`

**`clear_area(store)`**

- 前置：`store` 至少有 `MAX_ORDER + 1` 个元素。
- 行为：把 `free_area[0..MAX_ORDER]` 和 `total_free` 保存到参数中；重新初始化 16 个链表并把各阶 `nr_free`、`total_free` 置 0；清除 `pages[0..npage-nbase-1]` 的 `PG_property`。
- 返回：保存下来的 `total_free`。
- 用途：构造“没有任何空闲页”的隔离环境，便于测试分配失败和手工挂块。

**`restore_area(store, total_free_store)`**

- 前置：保存期间原空闲块节点的 `page_link` 未被破坏。
- 行为：把保存的 16 个 `free_area_t` 和 `total_free` 拷回；遍历每阶链表，对每个块头重新置 `PG_property`。
- 后置：恢复原来的空闲链表和计数；不改变引用计数和其他标志。
- 说明：它只恢复保存过的块头，不负责清除隔离期间新产生的块头标志；调用者需先清除这些标志，避免留下不可达的“幽灵”块头。

### 8. 一致性检查：`buddy_check_consistency()`

**行为**：

- 对每个阶 `k`，遍历 `free_area[k]`：每个节点必须带 `PG_property`，`property` 必须等于 `2^k`；所有节点的 `property` 之和必须等于该阶 `nr_free`。
- 把所有阶的 `nr_free` 累加，必须等于 `total_free`，也必须等于 `buddy_nr_free_pages()`。

**后置**：只读检查，不改变状态。它校验链表、块大小和计数，不校验地址对齐、范围或块之间的重叠。

### 9. 场景检查

**`buddy_basic_check()`**

- 分配 3 个单页，确认非空、互不相同、引用计数为 0、`page2pa()` 小于 `npage * PGSIZE`、都不是块头。
- 用 `clear_area()` 保存并清空状态后，`alloc_page()` 必须失败。
- 释放这 3 页后 `total_free == 3`；重新分配 3 页成功，再分配失败；释放 1 页后 `total_free == 1`，分配 1 页成功，再分配失败。
- 用 `restore_area()` 恢复现场，最后归还剩余的 `p`、`p1`、`p2`。

**`check_coalesce()`**

- 先分配一个 2 页块 `p2`；保存并清空 `free_area` 后，把 `p2` 手工挂到 `free_area[1]`，并同步该阶 `nr_free` 和 `total_free`。
- 连续两次分配 1 页，应得到该块的两半；按任意顺序释放后，`p2` 重新成为 2 页块头，`property == 2`。
- 恢复现场，清除 `p2` 的块头标志，用 `free_pages(p2, 2)` 归还。

**`check_split()`**

- 先分配一个 8 页块 `p8`；保存并清空后，把 `p8` 手工挂到 `free_area[3]`，并同步计数。
- 分配 1 页应返回 `p8`，且 `p8` 从阶 3 链表消失。
- 分裂出的 `p8 + 4`、`p8 + 2`、`p8 + 1` 应分别是 4 页、2 页、1 页块头，并分别位于阶 2、1、0 的链表中。
- 恢复现场后清除这些块头以及 `p8` 的标志，调用 `free_pages(p8, 1)` 归还 `p8` 首页。

**`check_non_power_of_two()`**

- 先分配一个 4 页块 `p4`；保存并清空后，把 `p4` 手工挂到 `free_area[2]`，并同步计数。
- 调用 `alloc_pages(3)` 应返回 `p4`；起始下标按 4 页对齐；`total_free` 减少 4 而不是 3；`p4` 及其后 3 页都不是块头；`free_area[2]` 为空。
- 再次调用 `alloc_pages(3)` 应失败。
- 恢复现场、清除标志后，用 `free_pages(p3, 4)` 按实际块大小归还。

**`buddy_check()`**

- 开始和结束各调用一次 `buddy_check_consistency()`。
- 核对初始状态下各阶 `nr_free` 之和等于 `total_free` 和 `buddy_nr_free_pages()`。
- 依次执行 `buddy_basic_check()`、1/2/4/8 页的分配与释放、`check_coalesce()`、`check_split()`、`check_non_power_of_two()`。
- 请求 `1UL << (MAX_ORDER + 1)` 页必须返回 `NULL`；最后再次核对计数和链表一致性。

### 10. 管理器注册与边界

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

- 所有成员必须非空。绑定为当前管理器后，`pmm_init()` 会调用 `init`、`init_memmap`，最后通过 `check_alloc_page()` 调用 `check`。
- `MAX_ORDER = 15` 对应最大 `2^15` 页，即 128 MB。`QEMU -m` 改变内存大小时需要同步调整该值。
- `n == 0` 由 `assert` 拒绝；`n > total_free`、请求阶超过 `MAX_ORDER`、或找不到足够大的空闲块时返回 `NULL`，且不改变状态。
- 稳定的不变量：所有阶 `nr_free` 之和等于 `total_free` 等于 `nr_free_pages()`；空闲块头满足 `PG_property == 1` 且 `property == 2^k`；已分配页不带 `PG_property`，引用计数为 0。
- 启动后应看到 `memory management: buddy_pmm_manager`，并在检查结束后输出 `check_alloc_page() succeeded!`，且没有断言失败或 panic。
