#include <slub.h>

#if ENABLE_SLUB_CHALLENGE

#include <assert.h>
#include <list.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>

#define SLUB_ALIGN       16
#define SLUB_CLASSES     8
#define SLUB_SMALL_MAX   2048
#define SLUB_BITMAP_SIZE (PGSIZE / SLUB_ALIGN / 8)
#define SLUB_ALIGN_UP(n) (((n) + SLUB_ALIGN - 1) & ~(size_t)(SLUB_ALIGN - 1))

/* Each cache owns ALL its slabs, including full slabs. A bitmap is kept
 * outside the payload: writing every byte of an object cannot corrupt the
 * allocation state. Unused bitmap bits and page-tail bytes are never slots.
 * No struct Page field is borrowed for SLUB bookkeeping.
 */
struct slub_slab {
    list_entry_t link;
    struct Page *page;
    size_t capacity;
    size_t inuse;
    uint8_t allocated[SLUB_BITMAP_SIZE];
};

struct slub_cache {
    size_t object_size;
    list_entry_t slabs;
};

/* Stored at the beginning of each large allocation's first page. The list
 * retains ownership without inspecting an untrusted pointer's memory.
 * Keep the exact PMM request so free_pages() gets the same base/count.
 */
struct slub_large {
    list_entry_t link;
    struct Page *page;
    size_t npages;
};

#define SLUB_DATA_OFFSET  SLUB_ALIGN_UP(sizeof(struct slub_slab))
#define SLUB_LARGE_OFFSET SLUB_ALIGN_UP(sizeof(struct slub_large))

static struct slub_cache slub_caches[SLUB_CLASSES];
static list_entry_t slub_large_blocks;
static bool slub_ready;

/* Test instrumentation is private and disappears with the extension.
 * The default false gate never changes PMM policy or its data structures.
 */
static bool slub_check_fail_pages;
static size_t slub_check_page_failures;
static size_t slub_init_pages_before, slub_init_pages_after;

/* Page layer: Page * is a descriptor, never the writable page contents.
 * PMM hands out pages within its existing kernel direct map. The matching
 * free uses only the saved descriptor/count, leaving PMM policy unchanged.
 */
static void *
slub_page_alloc(size_t npages, struct Page **owner) {
    if (slub_check_fail_pages) {
        slub_check_page_failures++;
        return NULL;
    }
    struct Page *page = alloc_pages(npages);
    if (page == NULL) {
        return NULL;
    }
    *owner = page;
    return (void *)(page2pa(page) + va_pa_offset);
}

/* Create one slab lazily; only its private management area is cleared. */
static struct slub_slab *
slub_new_slab(struct slub_cache *cache) {
    struct Page *page;
    struct slub_slab *slab = slub_page_alloc(1, &page);
    if (slab == NULL) {
        return NULL;
    }
    memset(slab, 0, sizeof(*slab));
    slab->page = page;
    slab->capacity = (PGSIZE - SLUB_DATA_OFFSET) / cache->object_size;
    list_add(&cache->slabs, &slab->link);
    return slab;
}

/* Reuse a free slot before asking PMM for another page. All arithmetic is
 * bounded by the capacity computed from this single page's actual layout.
 */
static void *
slub_small_alloc(struct slub_cache *cache) {
    struct slub_slab *slab = NULL;
    list_entry_t *le;
    for (le = list_next(&cache->slabs); le != &cache->slabs;
         le = list_next(le)) {
        struct slub_slab *candidate = to_struct(le, struct slub_slab, link);
        if (candidate->inuse < candidate->capacity) {
            slab = candidate;
            break;
        }
    }
    if (slab == NULL && (slab = slub_new_slab(cache)) == NULL) {
        return NULL;
    }
    for (size_t slot = 0; slot < slab->capacity; slot++) {
        uint8_t mask = 1U << (slot % 8);
        if (!(slab->allocated[slot / 8] & mask)) {
            slab->allocated[slot / 8] |= mask;
            slab->inuse++;
            return (char *)slab + SLUB_DATA_OFFSET + slot * cache->object_size;
        }
    }
    panic("SLUB: inconsistent slab bitmap/count");
}

/* Round without an unchecked size + PGSIZE - 1. Reject page counts whose
 * byte extent cannot be represented, before requesting any resources.
 */
static void *
slub_large_alloc(size_t size) {
    if (size > (size_t)-1 - SLUB_LARGE_OFFSET) {
        return NULL;
    }
    size_t bytes = size + SLUB_LARGE_OFFSET;
    size_t npages = bytes / PGSIZE + (bytes % PGSIZE != 0);
    if (npages > (size_t)-1 / PGSIZE || npages > nr_free_pages()) {
        return NULL;
    }
    struct Page *page;
    struct slub_large *block = slub_page_alloc(npages, &page);
    if (block == NULL) {
        return NULL;
    }
    block->page = page;
    block->npages = npages;
    list_add(&slub_large_blocks, &block->link);
    return (char *)block + SLUB_LARGE_OFFSET;
}

void
slub_init(void) {
    static_assert(SLUB_DATA_OFFSET < PGSIZE);
    static_assert(SLUB_DATA_OFFSET + SLUB_SMALL_MAX <= PGSIZE);
    static_assert(SLUB_BITMAP_SIZE * 8 >=
                  (PGSIZE - SLUB_DATA_OFFSET) / SLUB_ALIGN);
    static_assert(SLUB_LARGE_OFFSET < PGSIZE);
    assert(!slub_ready);
    slub_init_pages_before = nr_free_pages();
    for (size_t i = 0; i < SLUB_CLASSES; i++) {
        slub_caches[i].object_size = (size_t)SLUB_ALIGN << i;
        list_init(&slub_caches[i].slabs);
    }
    list_init(&slub_large_blocks);
    slub_ready = 1;
    slub_init_pages_after = nr_free_pages();
}

void *
kmalloc(size_t size) {
    assert(slub_ready);
    if (size == 0) {
        return NULL;
    }
    for (size_t i = 0; i < SLUB_CLASSES; i++) {
        if (size <= slub_caches[i].object_size) {
            return slub_small_alloc(&slub_caches[i]);
        }
    }
    return slub_large_alloc(size);
}

/* Validate every error condition before touching bitmap/count/list state.
 * This also catches management-area pointers, slot interiors, page tails,
 * and double frees while another object keeps the slab alive.
 */
static void
slub_small_free(struct slub_cache *cache, struct slub_slab *slab,
                uintptr_t address) {
    size_t offset = address - (uintptr_t)slab;
    if (offset < SLUB_DATA_OFFSET) {
        panic("SLUB: kfree points into slab management area");
    }
    offset -= SLUB_DATA_OFFSET;
    if (offset >= slab->capacity * cache->object_size) {
        panic("SLUB: kfree points into slab tail");
    }
    if (offset % cache->object_size != 0) {
        panic("SLUB: kfree points inside a slot");
    }
    size_t slot = offset / cache->object_size;
    uint8_t mask = 1U << (slot % 8);
    if (!(slab->allocated[slot / 8] & mask)) {
        panic("SLUB: kfree of an already free slot");
    }
    assert(slab->inuse > 0);
    slab->allocated[slot / 8] &= ~mask;
    slab->inuse--;
    if (slab->inuse == 0) {
        struct Page *page = slab->page;
        list_del(&slab->link);
        free_page(page);
        /* slab is no longer owned; never access it after free_page(). */
    }
}

void
kfree(void *ptr) {
    assert(slub_ready);
    if (ptr == NULL) {
        return;
    }
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t page_base = address & ~(uintptr_t)(PGSIZE - 1);
    list_entry_t *le;
    for (size_t i = 0; i < SLUB_CLASSES; i++) {
        struct slub_cache *cache = &slub_caches[i];
        for (le = list_next(&cache->slabs); le != &cache->slabs;
             le = list_next(le)) {
            struct slub_slab *slab = to_struct(le, struct slub_slab, link);
            if (page_base == (uintptr_t)slab) {
                slub_small_free(cache, slab, address);
                return;
            }
        }
    }
    for (le = list_next(&slub_large_blocks); le != &slub_large_blocks;
         le = list_next(le)) {
        struct slub_large *block = to_struct(le, struct slub_large, link);
        if (address >= (uintptr_t)block &&
            address - (uintptr_t)block < block->npages * PGSIZE) {
            if (address - (uintptr_t)block != SLUB_LARGE_OFFSET) {
                panic("SLUB: kfree is not the start of a large object");
            }
            struct Page *page = block->page;
            size_t npages = block->npages;
            list_del(&block->link);
            free_pages(page, npages);
            return; /* block's management record has also been reclaimed. */
        }
    }
    panic("SLUB: kfree pointer is not owned by this allocator");
}

/* Fixed records do not allocate from either layer being measured. */
#define SLUB_CHECK_RECORDS (2 * SLUB_BITMAP_SIZE * 8 + 1)
struct slub_check_record {
    void *ptr;
    size_t size;
    unsigned int seed;
};
static struct slub_check_record slub_check_records[SLUB_CHECK_RECORDS];
static uint8_t slub_check_page_snapshot[PGSIZE];
static struct slub_cache slub_check_cache_snapshot[SLUB_CLASSES];
static struct slub_large slub_check_large_snapshot;

static uint8_t
slub_check_pattern(unsigned int seed, size_t offset) {
    return (uint8_t)(seed + 17 * offset + (seed >> 8) * (offset + 1));
}

/* Write and later verify EVERY requested byte, including cross-page bytes. */
static void
slub_check_alloc(size_t index, size_t size) {
    assert(index < SLUB_CHECK_RECORDS && slub_check_records[index].ptr == NULL);
    struct slub_check_record *r = &slub_check_records[index];
    r->ptr = kmalloc(size);
    assert(r->ptr != NULL && ((uintptr_t)r->ptr % SLUB_ALIGN) == 0);
    r->size = size;
    r->seed = (unsigned int)index + 1;
    for (size_t i = 0; i < size; i++) {
        ((uint8_t *)r->ptr)[i] = slub_check_pattern(r->seed, i);
    }
}

static void
slub_check_free(size_t index) {
    assert(index < SLUB_CHECK_RECORDS && slub_check_records[index].ptr != NULL);
    kfree(slub_check_records[index].ptr);
    slub_check_records[index].ptr = NULL;
}

static size_t
slub_check_matches(uintptr_t address) {
    size_t count = 0;
    for (size_t i = 0; i < SLUB_CHECK_RECORDS; i++) {
        if (slub_check_records[i].ptr != NULL &&
            (uintptr_t)slub_check_records[i].ptr == address) {
            count++;
        }
    }
    return count;
}

/* Check descriptors without changing any PMM fields. */
static void
slub_check_page_owner(struct Page *page, size_t count, uintptr_t address) {
    assert(page >= pages && page < pages + (npage - nbase));
    assert(count > 0 && count <= (size_t)(pages + (npage - nbase) - page));
    assert(page2pa(page) + va_pa_offset == address);
    for (size_t i = 0; i < count; i++) {
        assert(!PageReserved(page + i) && !PageProperty(page + i));
    }
}

/* Cross-check three independent observations: live API results, internal
 * slot/owner records, and the number of pages missing from PMM. A bitmap
 * bit must correspond to exactly one test record, including in full slabs.
 */
static void
slub_check_state(size_t baseline) {
    size_t owned_pages = 0, owned_objects = 0, records = 0;
    for (size_t i = 0; i < SLUB_CHECK_RECORDS; i++) {
        struct slub_check_record *r = &slub_check_records[i];
        if (r->ptr == NULL) {
            continue;
        }
        records++;
        uintptr_t start = (uintptr_t)r->ptr;
        assert(start % SLUB_ALIGN == 0 && r->size <= (uintptr_t)-1 - start);
        for (size_t j = 0; j < r->size; j++) {
            assert(((uint8_t *)r->ptr)[j] == slub_check_pattern(r->seed, j));
        }
        for (size_t j = 0; j < i; j++) {
            struct slub_check_record *other = &slub_check_records[j];
            if (other->ptr != NULL) {
                uintptr_t b = (uintptr_t)other->ptr;
                assert(start + r->size <= b || b + other->size <= start);
            }
        }
    }
    for (size_t c = 0; c < SLUB_CLASSES; c++) {
        struct slub_cache *cache = &slub_caches[c];
        assert(cache->object_size == ((size_t)SLUB_ALIGN << c));
        list_entry_t *head = &cache->slabs;
        assert(head->next->prev == head && head->prev->next == head);
        for (list_entry_t *le = list_next(head); le != head; le = list_next(le)) {
            assert(++owned_pages <= baseline);
            assert(le->next->prev == le && le->prev->next == le);
            struct slub_slab *slab = to_struct(le, struct slub_slab, link);
            assert((uintptr_t)slab % PGSIZE == 0);
            slub_check_page_owner(slab->page, 1, (uintptr_t)slab);
            assert(slab->capacity == (PGSIZE - SLUB_DATA_OFFSET) / cache->object_size);
            assert(slab->capacity > 0 && slab->capacity <= SLUB_BITMAP_SIZE * 8);
            size_t used = 0, free_slots = 0;
            for (size_t slot = 0; slot < SLUB_BITMAP_SIZE * 8; slot++) {
                bool allocated = (slab->allocated[slot / 8] >> (slot % 8)) & 1;
                if (slot >= slab->capacity) {
                    assert(!allocated);
                    continue;
                }
                uintptr_t address = (uintptr_t)slab + SLUB_DATA_OFFSET +
                                    slot * cache->object_size;
                assert(address + cache->object_size <= (uintptr_t)slab + PGSIZE);
                assert(slub_check_matches(address) == (allocated ? 1 : 0));
                if (allocated) {
                    used++;
                    for (size_t i = 0; i < SLUB_CHECK_RECORDS; i++) {
                        struct slub_check_record *r = &slub_check_records[i];
                        if ((uintptr_t)r->ptr == address) {
                            assert(r->size > 0 && r->size <= cache->object_size);
                            assert(c == 0 || r->size > cache->object_size / 2);
                        }
                    }
                } else {
                    free_slots++;
                }
            }
            assert(used == slab->inuse && used > 0);
            assert(used + free_slots == slab->capacity);
            owned_objects += used;
        }
    }
    list_entry_t *head = &slub_large_blocks;
    assert(head->next->prev == head && head->prev->next == head);
    for (list_entry_t *le = list_next(head); le != head; le = list_next(le)) {
        assert(le->next->prev == le && le->prev->next == le);
        struct slub_large *block = to_struct(le, struct slub_large, link);
        assert((uintptr_t)block % PGSIZE == 0 && block->npages <= baseline);
        slub_check_page_owner(block->page, block->npages, (uintptr_t)block);
        uintptr_t payload = (uintptr_t)block + SLUB_LARGE_OFFSET;
        assert(slub_check_matches(payload) == 1);
        for (size_t i = 0; i < SLUB_CHECK_RECORDS; i++) {
            struct slub_check_record *r = &slub_check_records[i];
            if ((uintptr_t)r->ptr == payload) {
                assert(r->size > SLUB_SMALL_MAX);
                assert(r->size <= block->npages * PGSIZE - SLUB_LARGE_OFFSET);
                assert(block->npages == (r->size + SLUB_LARGE_OFFSET - 1) / PGSIZE + 1);
            }
        }
        owned_pages += block->npages;
        assert(owned_pages <= baseline);
        owned_objects++;
    }
    assert(owned_objects == records);
    assert(nr_free_pages() <= baseline && baseline - nr_free_pages() == owned_pages);
}

static void
slub_check_boundaries(size_t baseline) {
    static const size_t sizes[] = {
        1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
        255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049
    };
    size_t count = sizeof(sizes) / sizeof(sizes[0]);
    assert(kmalloc(0) == NULL);
    kfree(NULL);
    assert(nr_free_pages() == baseline);
    for (size_t i = 0; i < count; i++) {
        slub_check_alloc(i, sizes[i]);
    }
    slub_check_state(baseline);
    size_t before = nr_free_pages();
    size_t max = (size_t)-1;
    assert(kmalloc(max) == NULL);
    assert(kmalloc(max - SLUB_LARGE_OFFSET + 1) == NULL);
    assert(kmalloc(max - SLUB_LARGE_OFFSET) == NULL);
    assert(kmalloc(max - PGSIZE + 1) == NULL);
    assert(kmalloc((size_t)1 << 63) == NULL);
    assert(nr_free_pages() == before);
    slub_check_state(baseline);
    for (size_t i = 1; i < count; i += 2) {
        slub_check_free(i);
    }
    slub_check_state(baseline); /* all surviving payloads must be intact */
    for (size_t i = 0; i < count; i += 2) {
        slub_check_free(i);
    }
    slub_check_state(baseline);
    cprintf("[slub] boundaries, alignment, zero/NULL, overflow: PASS\n");
}

static void
slub_check_slabs(size_t baseline) {
    for (size_t c = 0; c < SLUB_CLASSES; c++) {
        size_t size = (size_t)SLUB_ALIGN << c;
        size_t capacity = (PGSIZE - SLUB_DATA_OFFSET) / size;
        size_t count = 2 * capacity + 1;
        assert(count <= SLUB_CHECK_RECORDS);
        for (size_t i = 0; i < capacity; i++) {
            slub_check_alloc(i, size);
        }
        assert(nr_free_pages() == baseline - 1);
        struct slub_slab *first = (void *)ROUNDDOWN((uintptr_t)slub_check_records[0].ptr, PGSIZE);
        assert(first->inuse == capacity && first->capacity == capacity);
        slub_check_state(baseline);
        if (capacity > 1) {
            size_t hole = capacity / 2;
            void *saved = slub_check_records[hole].ptr;
            slub_check_free(hole); /* the other objects keep this slab alive */
            slub_check_state(baseline);
            size_t failed = slub_check_page_failures;
            slub_check_fail_pages = 1;
            slub_check_alloc(hole, size);
            slub_check_fail_pages = 0;
            assert(slub_check_records[hole].ptr == saved);
            assert(slub_check_page_failures == failed);
            assert(nr_free_pages() == baseline - 1);
        } else {
            /* A one-slot slab cannot retain a second live object. */
            slub_check_free(0);
            assert(nr_free_pages() == baseline);
            slub_check_alloc(0, size);
        }
        slub_check_state(baseline);
        for (size_t i = capacity; i < count; i++) {
            slub_check_alloc(i, size);
            size_t expected = i < 2 * capacity ? 2 : 3;
            assert(nr_free_pages() == baseline - expected);
        }
        slub_check_state(baseline);
        /* Remove the oldest full slab, then the newest partial slab, while
         * another full slab remains live. Never inspect a returned page. */
        for (size_t i = 0; i < capacity; i++) {
            slub_check_free(i);
            assert(nr_free_pages() == baseline - (i + 1 == capacity ? 2 : 3));
        }
        slub_check_state(baseline);
        slub_check_free(count - 1);
        assert(nr_free_pages() == baseline - 1);
        slub_check_state(baseline);
        for (size_t i = capacity; i < 2 * capacity; i++) {
            slub_check_free(i);
            assert(nr_free_pages() == baseline - (i + 1 == 2 * capacity ? 0 : 1));
        }
        slub_check_state(baseline);
        cprintf("[slub] size %lu, capacity %lu: growth/reuse/reclaim PASS\n",
                (unsigned long)size, (unsigned long)capacity);
    }
}

static void
slub_check_large_objects(size_t baseline) {
    const size_t sizes[] = {
        2049, PGSIZE - SLUB_LARGE_OFFSET, PGSIZE - SLUB_LARGE_OFFSET + 1,
        4096, 5000, 2 * PGSIZE - SLUB_LARGE_OFFSET,
        2 * PGSIZE - SLUB_LARGE_OFFSET + 1, 8192, 16385
    };
    slub_check_alloc(0, 64); /* preserve a small object across every operation */
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t before = nr_free_pages();
        slub_check_alloc(1, sizes[i]);
        struct slub_large *block = (void *)((char *)slub_check_records[1].ptr - SLUB_LARGE_OFFSET);
        size_t offset = (uintptr_t)slub_check_records[1].ptr - (uintptr_t)block;
        assert(offset == sizeof(*block) || offset == SLUB_ALIGN_UP(sizeof(*block)));
        size_t expected = (sizes[i] + offset - 1) / PGSIZE + 1;
        assert(block->npages == expected && nr_free_pages() == before - expected);
        slub_check_alloc(2, sizes[i]);
        assert(nr_free_pages() == before - 2 * expected);
        slub_check_state(baseline);
        slub_check_free(1);
        assert(nr_free_pages() == before - expected);
        slub_check_state(baseline);
        slub_check_free(2);
        assert(nr_free_pages() == before);
    }
    slub_check_free(0);
    slub_check_state(baseline);
    cprintf("[slub] large objects, full payloads, page/header boundaries: PASS\n");
}

static void
slub_check_failures(size_t baseline) {
    size_t capacity = (PGSIZE - SLUB_DATA_OFFSET) / 32;
    for (size_t i = 0; i < capacity; i++) {
        slub_check_alloc(i, 32);
    }
    slub_check_alloc(capacity, 5000);
    slub_check_state(baseline);
    void *page = (void *)ROUNDDOWN((uintptr_t)slub_check_records[0].ptr, PGSIZE);
    struct slub_large *large = (void *)((char *)slub_check_records[capacity].ptr - SLUB_LARGE_OFFSET);
    memcpy(slub_check_page_snapshot, page, PGSIZE);
    memcpy(slub_check_cache_snapshot, slub_caches, sizeof(slub_caches));
    slub_check_large_snapshot = *large;
    size_t before = nr_free_pages();
    slub_check_page_failures = 0;
    slub_check_fail_pages = 1;
    assert(kmalloc(32) == NULL);   /* selected cache is full */
    assert(kmalloc(64) == NULL);   /* selected cache has no slab */
    assert(kmalloc(5000) == NULL); /* a dedicated block is required */
    assert(slub_check_page_failures == 3);
    assert(kmalloc(0) == NULL && kmalloc((size_t)-1) == NULL);
    assert(slub_check_page_failures == 3);
    assert(nr_free_pages() == before);
    assert(memcmp(slub_check_page_snapshot, page, PGSIZE) == 0);
    assert(memcmp(slub_check_cache_snapshot, slub_caches, sizeof(slub_caches)) == 0);
    assert(memcmp(&slub_check_large_snapshot, large, sizeof(*large)) == 0);
    slub_check_state(baseline);
    void *hole = slub_check_records[1].ptr;
    slub_check_free(1);
    slub_check_alloc(1, 32);
    assert(slub_check_records[1].ptr == hole && slub_check_page_failures == 3);
    assert(nr_free_pages() == before);
    slub_check_state(baseline);
    slub_check_fail_pages = 0;
    slub_check_alloc(capacity + 1, 32);
    slub_check_alloc(capacity + 2, 64);
    slub_check_alloc(capacity + 3, 5000);
    size_t recovery_pages = 2 + (5000 + SLUB_LARGE_OFFSET - 1) / PGSIZE + 1;
    assert(nr_free_pages() == before - recovery_pages);
    slub_check_state(baseline);
    for (size_t i = 0; i < capacity + 4; i++) {
        slub_check_free(i);
    }
    slub_check_state(baseline);
    slub_check_page_failures = 0;
    cprintf("[slub] injected page failures, unchanged live state, recovery: PASS\n");
}

static void
slub_check_mixed(size_t baseline) {
    uint32_t random = 0x20261024;
    for (size_t step = 0; step < 1000; step++) {
        random = random * 1664525U + 1013904223U;
        size_t index = (random >> 16) % 64;
        if (slub_check_records[index].ptr != NULL) {
            slub_check_state(baseline);
            slub_check_free(index);
        } else {
            random = random * 1664525U + 1013904223U;
            slub_check_alloc(index, (random >> 8) % 8193 + 1);
        }
        if (step % 32 == 0) {
            slub_check_state(baseline);
        }
    }
    slub_check_state(baseline);
    for (size_t i = 0; i < 64; i++) {
        if (slub_check_records[i].ptr != NULL) {
            slub_check_free(i);
        }
    }
    slub_check_state(baseline);
    cprintf("[slub] 1000 mixed operations, nonoverlap and live contents: PASS\n");
}

#if SLUB_CHECK_NEGATIVE
static void *volatile slub_check_bad_ptr;
static void *volatile slub_check_negative_page;
static volatile size_t slub_check_negative_free_pages;

/* A stable breakpoint for external verification of the panic path. */
static void __noinline
slub_check_negative_ready(void) {
    asm volatile("" ::: "memory");
}

static void
slub_check_negative(size_t baseline) {
    slub_check_alloc(0, 32);
    slub_check_alloc(1, 32);
    struct slub_slab *slab = (void *)ROUNDDOWN((uintptr_t)slub_check_records[0].ptr, PGSIZE);
    assert(ROUNDDOWN((uintptr_t)slub_check_records[1].ptr, PGSIZE) == (uintptr_t)slab);
#if SLUB_CHECK_NEGATIVE == 1
    slub_check_bad_ptr = slab;
#elif SLUB_CHECK_NEGATIVE == 2
    slub_check_bad_ptr = (char *)slub_check_records[0].ptr + 1;
#elif SLUB_CHECK_NEGATIVE == 3
    slub_check_bad_ptr = (char *)slab + SLUB_DATA_OFFSET + slab->capacity * 32;
    assert((uintptr_t)slub_check_bad_ptr < (uintptr_t)slab + PGSIZE);
#elif SLUB_CHECK_NEGATIVE == 4
    slub_check_bad_ptr = slub_check_records[0].ptr;
    slub_check_free(0); /* record 1 keeps the slab alive */
#endif
    slub_check_state(baseline);
    slub_check_negative_page = slab;
    slub_check_negative_free_pages = nr_free_pages();
    memcpy(slub_check_page_snapshot, slab, PGSIZE);
    memcpy(slub_check_cache_snapshot, slub_caches, sizeof(slub_caches));
    cprintf("[slub] negative mode %d: expecting kfree panic\n", SLUB_CHECK_NEGATIVE);
    slub_check_negative_ready();
    kfree(slub_check_bad_ptr);
    panic("SLUB negative test failed: invalid free returned");
}
#endif

void
slub_check(void) {
    assert(slub_ready && !slub_check_fail_pages);
    size_t baseline = nr_free_pages();
    assert(slub_init_pages_before == slub_init_pages_after);
    assert(baseline == slub_init_pages_after);
    slub_check_state(baseline);
    cprintf("[slub] initialization without object pages: PASS\n");
    slub_check_boundaries(baseline);
    slub_check_slabs(baseline);
    slub_check_large_objects(baseline);
    slub_check_failures(baseline);
    slub_check_mixed(baseline);
    slub_check_state(baseline);
    assert(!slub_check_fail_pages && nr_free_pages() == baseline);
#if SLUB_CHECK_NEGATIVE
    cprintf("[slub] positive checks complete; all test pages returned\n");
    slub_check_negative(baseline);
#else
    cprintf("[slub] ALL CHECKS PASSED; free pages restored: %lu\n",
            (unsigned long)baseline);
#endif
}

#endif /* ENABLE_SLUB_CHALLENGE */
