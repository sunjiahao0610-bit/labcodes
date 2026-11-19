#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

void
schedule(void) {
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        current->need_resched = 0;
        list_entry_t *start = (current == idleproc) ? &proc_list : &current->list_link;
        struct proc_struct *next = idleproc;
        for (list_entry_t *le = list_next(start); le != start; le = list_next(le)) {
            if (le != &proc_list) {
                struct proc_struct *candidate = le2proc(le, list_link);
                if (candidate->state == PROC_RUNNABLE) {
                    next = candidate;
                    break;
                }
            }
        }
        // A lone runnable caller must get the CPU back from the idle loop.
        if (next == idleproc && current != idleproc && current->state == PROC_RUNNABLE) {
            idleproc->need_resched = 1;
        }
        next->runs++;
        if (next != current) {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}

