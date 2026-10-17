# Lab2：实现 Best-Fit 页面分配

[PROMPT]

**任务**：请参考 `kern/mm/default_pmm.c` 中的 First-Fit 实现，帮我完成 `kern/mm/best_fit_pmm.c` 中 `LAB2` 和 `YOUR CODE` 标记处的代码。

Best-Fit 就是在所有能满足申请大小的空闲块中，选择最小的一块。比如申请 2 页，有 2 页和 5 页的空闲块，就选择 2 页的块。

**操作要求**：

1. 请直接修改项目文件，不要只给出代码。
2. 这个模块需要完成初始化、加入空闲页、分配、释放和查询空闲页数。请阅读 `pmm.h` 的 `struct pmm_manager`，自己设计需要的函数和变量，并定义 `best_fit_pmm_manager`。如果文件里已有部分代码，就在原来的基础上补全。
3. 不要修改 `basic_check()` 和 `best_fit_check()`，也不要修改其他测试文件。
4. 在 `kern/mm/pmm.c` 的 `init_pmm_manager()` 中，把当前管理器改成 `best_fit_pmm_manager`，否则运行的还是 First-Fit。
5. 主要修改这两个 `.c` 文件，必要时补充头文件声明。保留任务一的代码，其他模块不用改。算法不用特别优化，能正确通过测试即可。

**输出要求**：完成后简单说明改了哪里、怎么分配和释放页面，以及测试结果。如果没有运行成功，也请说明原因。

[RELY]

可以参考项目中已有的代码，不需要重新实现链表和地址转换函数。

- `kern/mm/default_pmm.c`：参考初始化、分配、释放和合并的做法。
- `kern/mm/pmm.h`：查看管理器接口，以及 `alloc_pages()`、`free_pages()` 等函数。
- `kern/mm/memlayout.h`：查看页面信息、空闲链表和页标志。
- `libs/list.h`：查看链表的初始化、插入、删除和遍历操作。
- `kern/mm/best_fit_pmm.c`：仔细阅读两个测试函数。
- `Makefile` 和 `tools/` 中的测试脚本：查看编译、运行和评分方法。

`pmm.h` 中的管理器结构如下（省略注释）：

```c
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};
```

`memlayout.h` 中可以用到这两个结构（省略注释）：

```c
struct Page {
    int ref;
    uint64_t flags;
    unsigned int property;
    list_entry_t page_link;
};

typedef struct {
    list_entry_t free_list;
    unsigned int nr_free;
} free_area_t;
```

几个字段的含义：

- `ref` 是页的引用计数，`flags` 保存页的标志。
- 空闲块的第一页叫块头，它的 `property` 表示这个块有多少页，`page_link` 用来连接空闲链表。
- `nr_free` 是空闲页的总数，不是空闲块的数量。
- `PageReserved()` 判断是不是保留页；`PageProperty()` 判断是不是空闲块头。分配出去的普通页不要设成保留页。
- 可以用 `SetPageProperty()`、`ClearPageProperty()` 设置和清除块头标志，用 `set_page_ref()` 设置引用计数，用 `le2page()` 从链表节点找到对应的页。具体用法以头文件为准。

测试会直接保存、清空和恢复 `free_list`、`nr_free`，所以实现中要保留这两个名称。`free_list` 要能作为 `list_entry_t` 保存和赋值，`nr_free` 要能用 `unsigned int` 保存和恢复。可以使用 `free_area_t` 加两个宏来实现，也可以自己设计。链表中只放空闲块头，正常情况下各块的页数加起来应等于 `nr_free`。测试清空链表后，不要通过其他变量继续访问旧的空闲块。

[GUARANTEE]

需要实现下面五项功能，函数名可以自己定。以下是一种命名方式；如果已有这些函数，就补全它们：

```c
static void best_fit_init(void);
static void best_fit_init_memmap(struct Page *base, size_t n);
static struct Page *best_fit_alloc_pages(size_t n);
static void best_fit_free_pages(struct Page *base, size_t n);
static size_t best_fit_nr_free_pages(void);
```

还需要完成：

1. 在 `best_fit_pmm.c` 中定义 `const struct pmm_manager best_fit_pmm_manager`，不能只写声明。
2. `.name` 设为 `"best_fit_pmm_manager"`，其他成员分别对应上面的管理函数，`.check` 对应已有的 `best_fit_check()`。
3. 修改 `pmm.c` 中的 `init_pmm_manager()`，设置 `pmm_manager = &best_fit_pmm_manager`，保留原来的名称输出和初始化调用。
4. 两个原有测试函数保持不变，让它们能正常使用 `free_list` 和 `nr_free`。

可以增加需要的辅助函数和变量，辅助函数设为 `static`。不要修改已有结构体和对外接口。

[SPECIFICATION]

## 1. 初始化：`init`

**Pre-Condition**：开始使用分配器，尚未加入空闲页。

**Post-Condition**：初始化空闲链表，将空闲页数设为 0。

## 2. 加入空闲页：`init_memmap(base, n)`

**Pre-Condition**：`n > 0`，从 `base` 开始的 `n` 页连续、有效，原来是保留页，尚未加入空闲链表。

**Post-Condition**：清除这些页的旧标志和属性，将引用计数设为 0；把它们作为空闲块加入链表，设置块头标志和块大小，空闲页数增加 `n`。保留链表中原有的空闲块，不处理范围外的页。

## 3. 分配页面：`alloc_pages(n)`

**Pre-Condition**：`n > 0`。

**Post-Condition**：从所有大小不少于 `n` 的空闲块中，选择最小的一块，返回其前 `n` 页的起始 `struct Page *`，空闲页数减少 `n`。同样大小的块可任选一个。

**Case 1**：块大小刚好等于 `n`，将整块从空闲链表中移除。

**Case 2**：块大小大于 `n`，分配前 `n` 页，把剩余部分作为新的空闲块留在链表中。

**Case 3**：链表为空，或没有足够大的连续块，返回 `NULL`，不改变原有状态。即使空闲页总数足够，也不能把不连续的块拼起来分配。

**Requirements**：分配出去的页不带空闲块头标志，引用计数为 0，不要将它们设为保留页。不能找到第一个够大的块就停止，除非它的大小刚好等于 `n`。

## 4. 释放页面：`free_pages(base, n)`

**Pre-Condition**：`n > 0`，从 `base` 开始的 `n` 页连续、有效、已分配，且不是保留页。调用方不会重复释放。

**Post-Condition**：将这些页恢复为空闲页，引用计数设为 0，加入空闲链表，空闲页数增加 `n`。如果与前后空闲块在物理地址上连续，就合并成一个块；两边都相邻时要全部合并。

**Requirements**：合并时更新块大小，移除多余节点和旧块头标志，不重复增加空闲页数。允许释放原分配范围的一部分，也允许将多个已分配的连续小块一起释放。

## 5. 查询空闲页数：`nr_free_pages`

**Pre-Condition**：分配器已初始化。

**Post-Condition**：返回 `nr_free`，不改变任何状态。返回的是空闲页数，不是空闲块数。

## 6. 注册并启用管理器

**Pre-Condition**：各管理函数已实现，原有两个测试函数保持不变。

**Post-Condition**：定义 `best_fit_pmm_manager`，将各成员绑定到对应函数，`.check` 绑定 `best_fit_check`。修改 `pmm.c` 的 `init_pmm_manager()`，设置 `pmm_manager = &best_fit_pmm_manager`，保留名称输出和初始化调用。

## 7. 测试与验收

**Requirements**：

- 兼容测试对 `free_list`、`nr_free` 的保存、清空和恢复。特别是链表为空、计数尚未清零时，分配也必须返回 `NULL`，不能触发断言或访问原来的空闲块。
- 通过原有 `basic_check()` 和 `best_fit_check()`。例如同时有两页块和一页块时，申请一页应优先选择一页块，留下两页块供后续申请。
- 依次执行 `make clean`、`make -j2`，再用 `make qemu` 检查启动输出：管理器名称应为 `best_fit_pmm_manager`，并出现 `check_alloc_page() succeeded!`，没有断言失败或 panic。内核检查后会一直循环，可用 `timeout 10s make qemu` 收集日志，按日志判断结果。
- 执行 `make grade`，确认两项测试均为 `PASS`。其中第二项只检查管理器是否切换，不能替代前面的算法检查。缺少运行工具或测试失败时，如实说明，不修改测试来通过检查。
