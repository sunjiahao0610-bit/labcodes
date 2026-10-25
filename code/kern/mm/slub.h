#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

/* Optional Lab2 extension. Pass DEFS=-DENABLE_SLUB_CHALLENGE=1 to make.
 * Use separate OBJDIR/BINDIR directories for each configuration, or do a
 * clean build when switching. An undefined switch and an explicit 0 both
 * remove the implementation, state, declarations and initialization call.
 */
#ifndef ENABLE_SLUB_CHALLENGE
#define ENABLE_SLUB_CHALLENGE 0
#endif

#if ENABLE_SLUB_CHALLENGE != 0 && ENABLE_SLUB_CHALLENGE != 1
#error "ENABLE_SLUB_CHALLENGE must be 0 or 1"
#endif

#if ENABLE_SLUB_CHALLENGE

#include <defs.h>

/* Independent panic runs: 0 = positive checks only (default), 1 = slab
 * management area, 2 = slot interior, 3 = in-page tail slot, 4 = double free.
 * Each nonzero mode completes the positive suite before provoking its panic.
 * Give every mode a separate OBJDIR/BINDIR, just like the main switch.
 */
#ifndef SLUB_CHECK_NEGATIVE
#define SLUB_CHECK_NEGATIVE 0
#endif
#if SLUB_CHECK_NEGATIVE < 0 || SLUB_CHECK_NEGATIVE > 4
#error "SLUB_CHECK_NEGATIVE must be between 0 and 4"
#endif

/* Single CPU, serialized calls only; no interrupt/concurrent callers.
 * Initialize exactly once, after pmm_init() and its checks. Initialization
 * takes no PMM pages and does not call kmalloc().
 */
void slub_init(void);

/* Return >= size writable bytes, aligned to at least 16 bytes, or NULL for
 * zero size, overflow or allocation failure. Contents are not zeroed.
 * Requests <= 2048 use the smallest of 16/32/64/128/256/512/1024/2048;
 * larger requests own a contiguous PMM block including a private header.
 * Requires slub_init(). The caller owns only the returned payload.
 */
void *kmalloc(size_t size);

/* NULL is a no-op. Otherwise pass a live, original kmalloc() result.
 * The last small object immediately returns its slab page to PMM; a large
 * object returns its whole block. Recognizable invalid frees in a live slab
 * panic before changing allocation state. Arbitrary/stale pointers are not
 * a supported recovery interface. Requires slub_init().
 */
void kfree(void *ptr);

/* Startup self-check, called immediately after slub_init(), before clients
 * use the allocator. Uses fixed test storage; the positive run releases all
 * its objects and restores the initial free-page count. Nonzero negative
 * modes deliberately panic with another object keeping the slab alive.
 */
void slub_check(void);

#endif /* ENABLE_SLUB_CHALLENGE */
#endif /* !__KERN_MM_SLUB_H__ */
