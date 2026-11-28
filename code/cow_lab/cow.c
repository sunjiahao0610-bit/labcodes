#include <cow.h>
#include <assert.h>
#include <error.h>
#include <pmm.h>
#include <string.h>

/* Sv39 reserves bits 8 and 9 for software. */
#define PTE_COW 0x100

int cow_copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));

    while (start < end)
    {
        pte_t *parent = get_pte(from, start, 0);
        if (parent == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }

        pte_t entry = *parent;
        if (entry & PTE_V)
        {
            uint32_t perm = entry & (PTE_USER | PTE_COW);
            if (entry & PTE_W)
            {
                perm = (perm & ~PTE_W) | PTE_COW;
            }
            int ret = page_insert(to, pte2page(entry), start, perm);
            if (ret != 0)
            {
                return ret;
            }

            /* The child stays unpublished until all mappings are installed. */
            if (entry & PTE_W)
            {
                *parent = (entry & ~PTE_W) | PTE_COW;
                tlb_invalidate(from, start);
            }
        }
        start += PGSIZE;
    }
    return 0;
}

int cow_handle_store_fault(struct mm_struct *mm, uintptr_t addr)
{
    if (mm == NULL || !user_mem_check(mm, addr, 1, 1))
    {
        return -E_FAULT;
    }

    /* Validate the current leaf and finish the replacement without scheduling
     * another process. This serializes the operation on the lab's one hart. */
    pte_t *ptep = get_pte(mm->pgdir, addr, 0);
    const pte_t required = PTE_V | PTE_U | PTE_R | PTE_COW;
    if (ptep == NULL || (*ptep & (required | PTE_W)) != required)
    {
        return -E_FAULT;
    }

    struct Page *page = pte2page(*ptep);
    uint32_t perm = (*ptep & PTE_USER) | PTE_W;
    if (page_ref(page) == 1)
    {
        return page_insert(mm->pgdir, page, addr, perm);
    }

    struct Page *copy = alloc_page();
    if (copy == NULL)
    {
        return -E_NO_MEM;
    }
    memcpy(page2kva(copy), page2kva(page), PGSIZE);
    int ret = page_insert(mm->pgdir, copy, addr, perm);
    if (ret != 0)
    {
        free_page(copy);
    }
    return ret;
}

bool cow_copy_to_user(struct mm_struct *mm, void *dst, const void *src, size_t len)
{
    if (mm == NULL)
    {
        return copy_to_user(mm, dst, src, len);
    }

    uintptr_t addr = (uintptr_t)dst;
    bool intr_flag;
    local_intr_save(intr_flag);
    bool writable = user_mem_check(mm, addr, len, 1);
    uintptr_t end = addr + len;
    for (; writable && addr < end; addr = ROUNDDOWN(addr, PGSIZE) + PGSIZE)
    {
        pte_t *ptep = get_pte(mm->pgdir, addr, 0);
        const pte_t required = PTE_V | PTE_U | PTE_R | PTE_W;
        if (ptep == NULL || (*ptep & required) != required)
        {
            writable = (cow_handle_store_fault(mm, addr) == 0);
        }
    }
    if (writable)
    {
        memcpy(dst, src, len);
    }
    local_intr_restore(intr_flag);
    return writable;
}
