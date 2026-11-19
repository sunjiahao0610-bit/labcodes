#include <defs.h>
#include <stdio.h>
#include <string.h>
#include <error.h>
#include <list.h>
#include <pmm.h>
#include <vmm.h>
#include <swap.h>
#include <swap_clock.h>
#include <file.h>
#include <test_lab9.h>

#define TEST_MMAP_BASE             0x60000000UL
#define TEST_MMAP_SMALL_HOLE       0x60021000UL
#define TEST_MMAP_FIXED            0x60030000UL
#define TEST_MMAP_FILE             0x60040000UL
#define TEST_MUNMAP_FULL           0x60050000UL
#define TEST_MUNMAP_PARTIAL        0x60060000UL
#define TEST_MUNMAP_ANON           0x60070000UL

static char test_clock_pass[] = {
    '[', 'T', 'e', 's', 't', ' ', '1', ']', ' ', 'C', 'l', 'o', 'c', 'k',
    ' ', 'P', 'a', 'g', 'e', ' ', 'R', 'e', 'p', 'l', 'a', 'c', 'e', 'm',
    'e', 'n', 't', ':', ' ', 'P', 'A', 'S', 'S', ' ', '(', '4', '0', '/',
    '4', '0', ')', 10, 0
};

static char test_clock_fail[] = {
    '[', 'T', 'e', 's', 't', ' ', '1', ']', ' ', 'C', 'l', 'o', 'c', 'k',
    ' ', 'P', 'a', 'g', 'e', ' ', 'R', 'e', 'p', 'l', 'a', 'c', 'e', 'm',
    'e', 'n', 't', ':', ' ', 'F', 'A', 'I', 'L', 10, 0
};

static char test_mmap_pass[] = {
    '[', 'T', 'e', 's', 't', ' ', '2', ']', ' ', 'V', 'M', 'A', ' ', 'm',
    'm', 'a', 'p', ':', ' ', 'P', 'A', 'S', 'S', ' ', '(', '3', '0', '/',
    '3', '0', ')', 10, 0
};

static char test_mmap_fail[] = {
    '[', 'T', 'e', 's', 't', ' ', '2', ']', ' ', 'V', 'M', 'A', ' ', 'm',
    'm', 'a', 'p', ':', ' ', 'F', 'A', 'I', 'L', 10, 0
};

static char test_munmap_pass[] = {
    '[', 'T', 'e', 's', 't', ' ', '3', ']', ' ', 'V', 'M', 'A', ' ', 'm',
    'u', 'n', 'm', 'a', 'p', ':', ' ', 'P', 'A', 'S', 'S', ' ', '(', '3',
    '0', '/', '3', '0', ')', 10, 0
};

static char test_munmap_fail[] = {
    '[', 'T', 'e', 's', 't', ' ', '3', ']', ' ', 'V', 'M', 'A', ' ', 'm',
    'u', 'n', 'm', 'a', 'p', ':', ' ', 'F', 'A', 'I', 'L', 10, 0
};

static int
test_page_index(struct Page **pages, int count, struct Page *page)
{
    int i;
    for (i = 0; i < count; i++) {
        if (pages[i] == page) {
            return i;
        }
    }
    return -1;
}

static struct vma_struct *
test_insert_vma(struct mm_struct *mm, uintptr_t start, uintptr_t end,
                uint32_t flags, struct file *file, int pgoff)
{
    struct vma_struct *vma = vma_create(start, end, flags);
    if (vma == NULL) {
        return NULL;
    }
    vma->vm_file = file;
    vma->vm_pgoff = pgoff;
    if (file != NULL) {
        fopen_count_inc(file);
    }
    insert_vma_struct(mm, vma);
    return vma;
}

void
test_lab9_clock(void)
{
    struct mm_struct *mm = NULL;
    struct swap_manager *sm = &swap_manager_clock;
    struct Page *pages[3] = {NULL, NULL, NULL};
    struct Page *victim = NULL;
    bool active[3] = {0, 0, 0};
    int ok = 1, i, index, removed = 0;

    mm = mm_create();
    if (mm == NULL) {
        ok = 0;
        goto out;
    }

    if (sm->init_mm(mm) != 0) {
        ok = 0;
        goto out;
    }

    victim = NULL;
    if (sm->swap_out_victim(mm, &victim, 0) == 0 || victim != NULL) {
        ok = 0;
        goto out;
    }

    for (i = 0; i < 3; i++) {
        pages[i] = alloc_page();
        if (pages[i] == NULL) {
            ok = 0;
            goto out;
        }
        pages[i]->visited = 0;
    }

    for (i = 0; i < 3; i++) {
        if (sm->map_swappable(mm, 0x1000 + i * PGSIZE, pages[i], 0) != 0) {
            ok = 0;
            goto out;
        }
        active[i] = 1;
        if (pages[i]->visited == 0) {
            ok = 0;
            goto out;
        }
    }

    /* A referenced page gets a second chance instead of becoming the victim. */
    pages[0]->visited = 1;
    pages[1]->visited = 0;
    pages[2]->visited = 1;

    victim = NULL;
    if (sm->swap_out_victim(mm, &victim, 0) != 0 || victim != pages[1]) {
        ok = 0;
    }
    index = test_page_index(pages, 3, victim);
    if (index >= 0) {
        active[index] = 0;
    }
    if (pages[0]->visited != 0 && pages[2]->visited != 0) {
        ok = 0;
    }

    /* All remaining pages are referenced: the scan must clear and wrap around. */
    for (i = 0; i < 3; i++) {
        if (active[i]) {
            pages[i]->visited = 1;
        }
    }
    victim = NULL;
    if (sm->swap_out_victim(mm, &victim, 0) != 0) {
        ok = 0;
    }
    index = test_page_index(pages, 3, victim);
    if (index < 0 || !active[index] || victim->visited != 0) {
        ok = 0;
    } else {
        active[index] = 0;
        removed++;
    }

    while (removed < 3) {
        victim = NULL;
        if (sm->swap_out_victim(mm, &victim, 0) != 0) {
            break;
        }
        index = test_page_index(pages, 3, victim);
        if (index < 0 || !active[index]) {
            ok = 0;
            break;
        }
        active[index] = 0;
        removed++;
    }

    /* Re-register pages through the swap-in path. */
    for (i = 0; i < 3; i++) {
        if (sm->map_swappable(mm, 0x1000 + i * PGSIZE, pages[i], 1) != 0 ||
            pages[i]->visited == 0) {
            ok = 0;
        }
        active[i] = 1;
    }

    removed = 0;
    while (removed < 3) {
        victim = NULL;
        if (sm->swap_out_victim(mm, &victim, 0) != 0) {
            break;
        }
        index = test_page_index(pages, 3, victim);
        if (index < 0 || !active[index]) {
            ok = 0;
            break;
        }
        active[index] = 0;
        removed++;
    }

    victim = (struct Page *)0x1;
    if (sm->swap_out_victim(mm, &victim, 0) == 0 ||
        victim != (struct Page *)0x1) {
        ok = 0;
    }

out:
    for (i = 0; i < 3; i++) {
        if (active[i] && pages[i] != NULL) {
            list_del(&(pages[i]->pra_page_link));
        }
    }
    for (i = 0; i < 3; i++) {
        if (pages[i] != NULL) {
            free_page(pages[i]);
        }
    }
    if (mm != NULL) {
        mm_destroy(mm);
    }

    if (ok) {
        cprintf(test_clock_pass);
    } else {
        cprintf(test_clock_fail);
    }
}

void
test_lab9_mmap(void)
{
    struct mm_struct *mm = NULL;
    struct vma_struct *vma = NULL;
    struct file file;
    pte_t *ptep;
    uintptr_t ret;
    int ok = 1;
    int count;

    mm = mm_create();
    if (mm == NULL) {
        ok = 0;
        goto out;
    }
    mm->pgdir = boot_pgdir_va;

    if (test_insert_vma(mm, TEST_MMAP_BASE, TEST_MMAP_BASE + PGSIZE,
                        VM_READ, NULL, 0) == NULL ||
        test_insert_vma(mm, TEST_MMAP_BASE + 0x20000,
                        TEST_MMAP_BASE + 0x21000, VM_READ, NULL, 0) == NULL ||
        test_insert_vma(mm, TEST_MMAP_BASE + 0x22000,
                        TEST_MMAP_BASE + 0x23000, VM_READ, NULL, 0) == NULL) {
        ok = 0;
        goto out;
    }

    /* The one-page gap is a better fit than the preceding large gap. */
    ret = do_mmap(mm, 0, PGSIZE, VM_READ | VM_WRITE, NULL, 0);
    if (ret != TEST_MMAP_SMALL_HOLE || mm->map_count != 4) {
        ok = 0;
        goto out;
    }

    vma = find_vma(mm, TEST_MMAP_SMALL_HOLE);
    if (vma == NULL || vma->vm_start != TEST_MMAP_SMALL_HOLE ||
        vma->vm_end != TEST_MMAP_SMALL_HOLE + PGSIZE ||
        vma->vm_flags != (VM_READ | VM_WRITE) ||
        vma->vm_file != NULL || vma->vm_pgoff != 0) {
        ok = 0;
        goto out;
    }

    /* Fixed addresses are page aligned at both ends. */
    ret = do_mmap(mm, TEST_MMAP_FIXED + 0x10, PGSIZE + 0x10,
                  VM_READ, NULL, 0);
    if (ret != TEST_MMAP_FIXED || mm->map_count != 5) {
        ok = 0;
        goto out;
    }
    vma = find_vma(mm, TEST_MMAP_FIXED);
    if (vma == NULL || vma->vm_start != TEST_MMAP_FIXED ||
        vma->vm_end != TEST_MMAP_FIXED + 2 * PGSIZE) {
        ok = 0;
        goto out;
    }

    count = mm->map_count;

    memset(&file, 0, sizeof(file));
    file.status = FD_OPENED;
    file.readable = 1;
    file.fd = 0;
    file.open_count = 1;

    ret = do_mmap(mm, TEST_MMAP_FIXED, PGSIZE, 0x80000000, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }
    ret = do_mmap(mm, TEST_MMAP_FIXED, PGSIZE, VM_READ, &file, -PGSIZE);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count ||
        fopen_count(&file) != 1) {
        ok = 0;
        goto out;
    }
    ret = do_mmap(mm, TEST_MMAP_FIXED, PGSIZE, VM_READ, &file, 1);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count ||
        fopen_count(&file) != 1) {
        ok = 0;
        goto out;
    }
    ret = do_mmap(mm, USERTOP - 1, 2, VM_READ, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }

    ptep = get_pte(mm->pgdir, TEST_MMAP_FIXED, 0);
    if (ptep != NULL && (*ptep & PTE_V)) {
        ok = 0;
        goto out;
    }

    /* Crossing into an existing VMA is invalid. */
    ret = do_mmap(mm, TEST_MMAP_BASE + 0x1f000, 2 * PGSIZE,
                  VM_READ, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }

    ret = do_mmap(mm, USERBASE - PGSIZE, PGSIZE, VM_READ, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }
    ret = do_mmap(mm, USERTOP - PGSIZE, 2 * PGSIZE, VM_READ, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }
    ret = do_mmap(mm, TEST_MMAP_BASE, 0, VM_READ, NULL, 0);
    if ((intptr_t)ret != -E_INVAL || mm->map_count != count) {
        ok = 0;
        goto out;
    }

    memset(&file, 0, sizeof(file));
    file.status = FD_OPENED;
    file.readable = 1;
    file.fd = 0;
    file.open_count = 1;

    ret = do_mmap(mm, TEST_MMAP_FILE, 2 * PGSIZE, VM_READ,
                  &file, 5 * PGSIZE);
    vma = find_vma(mm, TEST_MMAP_FILE);
    if (ret != TEST_MMAP_FILE || vma == NULL ||
        vma->vm_file != &file || vma->vm_pgoff != 5 ||
        fopen_count(&file) != 2 || mm->map_count != count + 1) {
        ok = 0;
        goto out;
    }

out:
    if (mm != NULL) {
        mm_destroy(mm);
    }
    if (ok) {
        cprintf(test_mmap_pass);
    } else {
        cprintf(test_mmap_fail);
    }
}

static int
test_munmap_full(void)
{
    struct mm_struct *mm = NULL;
    struct vma_struct *vma;
    struct vma_struct *neighbor;
    struct file file;
    struct Page *pages[2] = {NULL, NULL};
    bool inserted[2] = {0, 0};
    size_t nr_free_store = 0;
    uintptr_t start = TEST_MUNMAP_FULL;
    uintptr_t end = start + 2 * PGSIZE;
    uintptr_t neighbor_start = end + 2 * PGSIZE;
    pte_t *ptep;
    int ok = 1, i;

    mm = mm_create();
    if (mm == NULL) {
        return 0;
    }
    mm->pgdir = boot_pgdir_va;

    memset(&file, 0, sizeof(file));
    file.status = FD_OPENED;
    file.readable = 1;
    file.fd = 0;
    file.open_count = 1;

    vma = test_insert_vma(mm, start, end, VM_READ | VM_WRITE,
                          &file, 7);
    if (vma == NULL || fopen_count(&file) != 2) {
        ok = 0;
        goto out;
    }
    neighbor = test_insert_vma(mm, neighbor_start, neighbor_start + PGSIZE,
                               VM_READ, NULL, 0);
    if (neighbor == NULL) {
        ok = 0;
        goto out;
    }
    nr_free_store = nr_free_pages();

    for (i = 0; i < 2; i++) {
        pages[i] = alloc_page();
        if (pages[i] == NULL ||
            page_insert(mm->pgdir, pages[i], start + i * PGSIZE,
                        PTE_U | PTE_R | PTE_W) != 0) {
            ok = 0;
            goto out;
        }
        inserted[i] = 1;
    }

    mm->mmap_cache = vma;
    if (do_munmap(mm, start + 0x100, (end - start) - 0x100) != 0) {
        ok = 0;
        goto out;
    }

    if (mm->mmap_cache != NULL ||
        find_vma(mm, start) != NULL ||
        find_vma(mm, neighbor_start) != neighbor ||
        mm->map_count != 1 || fopen_count(&file) != 1) {
        ok = 0;
        goto out;
    }

    for (i = 0; i < 2; i++) {
        ptep = get_pte(mm->pgdir, start + i * PGSIZE, 0);
        if ((ptep != NULL && (*ptep & PTE_V)) || page_ref(pages[i]) != 0) {
            ok = 0;
            goto out;
        }
    }

out:
    if (mm != NULL && mm->pgdir != NULL) {
        unmap_range(mm->pgdir, start, end);
        exit_range(mm->pgdir, start, end);
        if (vma != NULL && nr_free_pages() != nr_free_store) {
            ok = 0;
        }
    }
    for (i = 0; i < 2; i++) {
        if (pages[i] != NULL && !inserted[i]) {
            free_page(pages[i]);
        }
    }
    if (mm != NULL) {
        mm_destroy(mm);
    }
    return ok;
}

static int
test_munmap_partial(void)
{
    struct mm_struct *mm = NULL;
    struct vma_struct *vma;
    struct file file;
    struct Page *page = NULL;
    size_t nr_free_store = 0;
    uintptr_t start = TEST_MUNMAP_PARTIAL;
    uintptr_t end = start + 3 * PGSIZE;
    pte_t *ptep;
    int ok = 1;

    mm = mm_create();
    if (mm == NULL) {
        return 0;
    }
    mm->pgdir = boot_pgdir_va;

    memset(&file, 0, sizeof(file));
    file.status = FD_OPENED;
    file.readable = 1;
    file.fd = 0;
    file.open_count = 1;

    vma = test_insert_vma(mm, start, end, VM_READ | VM_WRITE,
                          &file, 3);
    if (vma == NULL || fopen_count(&file) != 2) {
        ok = 0;
        goto out;
    }
    nr_free_store = nr_free_pages();

    page = alloc_page();
    if (page == NULL ||
        page_insert(mm->pgdir, page, start, PTE_U | PTE_R | PTE_W) != 0) {
        ok = 0;
        goto out;
    }

    /* A partial unmapping must not delete the VMA or touch its pages. */
    if (do_munmap(mm, start, 2 * PGSIZE) == 0 ||
        find_vma(mm, start) != vma || mm->map_count != 1 ||
        fopen_count(&file) != 2) {
        ok = 0;
        goto out;
    }

    ptep = get_pte(mm->pgdir, start, 0);
    if (ptep == NULL || !(*ptep & PTE_V) || page_ref(page) != 1) {
        ok = 0;
        goto out;
    }
    if (do_munmap(mm, end + PGSIZE, PGSIZE) == 0 ||
        do_munmap(mm, start, 0) == 0 ||
        do_munmap(mm, USERBASE - PGSIZE, PGSIZE) == 0 ||
        do_munmap(mm, USERTOP - 1, 2) == 0) {
        ok = 0;
        goto out;
    }

out:
    if (mm != NULL && mm->pgdir != NULL) {
        unmap_range(mm->pgdir, start, end);
        exit_range(mm->pgdir, start, end);
        if (vma != NULL && nr_free_pages() != nr_free_store) {
            ok = 0;
        }
    } else if (page != NULL) {
        free_page(page);
    }
    if (mm != NULL) {
        mm_destroy(mm);
    }
    return ok;
}

static int
test_munmap_anonymous(void)
{
    struct mm_struct *mm = NULL;
    struct vma_struct *vma;
    struct Page *page = NULL;
    pte_t *ptep;
    uintptr_t start = TEST_MUNMAP_ANON;
    uintptr_t end = start + PGSIZE;
    bool mapped = 0;
    int ok = 1;

    mm = mm_create();
    if (mm == NULL) {
        return 0;
    }
    mm->pgdir = boot_pgdir_va;

    vma = test_insert_vma(mm, start, end, VM_READ | VM_WRITE, NULL, 0);
    if (vma == NULL) {
        ok = 0;
        goto out;
    }

    page = alloc_page();
    if (page == NULL ||
        page_insert(mm->pgdir, page, start, PTE_U | PTE_R | PTE_W) != 0) {
        ok = 0;
        goto out;
    }
    mapped = 1;
    mm->mmap_cache = vma;

    if (do_munmap(mm, start, PGSIZE) != 0 || mm->map_count != 0 ||
        mm->mmap_cache != NULL || page_ref(page) != 0) {
        ok = 0;
        goto out;
    }
    ptep = get_pte(mm->pgdir, start, 0);
    if (ptep != NULL && (*ptep & PTE_V)) {
        ok = 0;
    }

out:
    if (mm != NULL && mm->pgdir != NULL) {
        unmap_range(mm->pgdir, start, end);
        exit_range(mm->pgdir, start, end);
    }
    if (page != NULL && !mapped) {
        free_page(page);
    }
    if (mm != NULL) {
        mm_destroy(mm);
    }
    return ok;
}

void
test_lab9_munmap(void)
{
    int ok = 1;

    if (!test_munmap_full()) {
        ok = 0;
    }
    if (!test_munmap_partial()) {
        ok = 0;
    }
    if (!test_munmap_anonymous()) {
        ok = 0;
    }

    if (ok) {
        cprintf(test_munmap_pass);
    } else {
        cprintf(test_munmap_fail);
    }
}
