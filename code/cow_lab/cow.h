#ifndef LAB5_COW_H
#define LAB5_COW_H

#include <vmm.h>

int cow_copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end);
/* The store exception entry calls this with local interrupts disabled. */
int cow_handle_store_fault(struct mm_struct *mm, uintptr_t addr);
bool cow_copy_to_user(struct mm_struct *mm, void *dst, const void *src, size_t len);

#endif
