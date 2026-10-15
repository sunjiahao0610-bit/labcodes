# first-fit 算法实验报告

## first-fit 具体实现
`default_init()`：初始化 `free_list`。

`default_init_memmap(struct Page *base, size_t n)`：
1. 遍历每一页，重置状态。
2. 将每一组连续页的首页加入 `free_list`，并以其物理地址大小顺序排列。

`default_alloc_pages(size_t n)`：
1. 寻找不少于 $n$ 页的连续页的首页(长度为 $x$)。
2. 如果没找到，返回 `NULL`，否则将该连续页拆分为 $n$ 和 $x-n$ 页。
3. 将 $x-n$ 连续页重新加入 `free_list`，从中删除 $n$ 连续页，并返回其首页。

`default_free_pages`：
1. 检查欲释放页是否合法。
2. 插入 `free_list`。
3. 进行向前和向后合并。

## 改进空间
1. 提速度：当前分配/释放都是 O(n) 线性扫描，可以改用伙伴系统（O(log n)，合并无需查找）或分级空闲链表（O(1)）；next-fit 能减少扫描但会加重碎片。
2. 减碎片：加分裂阈值（剩余块太小就不切分）、延迟合并（释放时先不合并，分配失败再统一合并）。