# Challenge 实验报告
1. 说明语句 local_intr_save(intr_flag); ... local_intr_restore(intr_flag); 是如何实现开关中断的。 
```C
// sync.h
static inline bool __intr_save(void) {
    if (read_csr(sstatus) & SSTATUS_SIE) {
        intr_disable();
        return 1;
    }
    return 0;
}

static inline void __intr_restore(bool flag) {
    if (flag) {
        intr_enable();
    }
}

#define local_intr_save(x) \
    do {                   \
        x = __intr_save(); \
    } while (0)
#define local_intr_restore(x) __intr_restore(x);

// intr.c
/* intr_enable - enable irq interrupt */
void intr_enable(void) { set_csr(sstatus, SSTATUS_SIE); }

/* intr_disable - disable irq interrupt */
void intr_disable(void) { clear_csr(sstatus, SSTATUS_SIE); }
```
通过设置`sstatus`寄存器实现开关终端。

---

2. `get_pte()` 函数中有两段形式类似的代码，结合 sv32、sv39、sv48 的异同，解释这两段代码为什么如此相像。 

两段除了"索引用的 VPN 段"和"页表基址来源"不同，其余完全一样：都是"用 VPN 索引 --> 查 PTE_V --> 无效则分配清零挂上 --> KADDR 转虚拟地址继续"。


sv32、sv39、sv48每级页表都是 4KB，每一级的遍历动作完全相同，只有 VPN 段的划分和级数不同。

---

3. 目前 `get_pte()` 函数将页表项查找和页表页分配合并在一个函数里，你认为这种写法好吗？有没有必要把两个功能拆开？

这种写法调用方便，但是职责不单一，可读性差，且不容易做"纯查询"优化，所以有必要把两个功能拆开。