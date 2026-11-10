#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <stdio.h>
#include <trap.h>
#include <vmm.h>

#define TICK_NUM 100

static void print_ticks()
{
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S
 */
void idt_init(void)
{
    extern void __alltraps(void);
    /* Set sscratch register to 0, indicating to exception vector that we are
     * presently executing in the kernel */
    write_csr(sscratch, 0);
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps);
    /* Allow kernel to access user memory */
    set_csr(sstatus, SSTATUS_SUM);
}

void print_trapframe(struct trapframe *tf)
{
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr)
{
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

extern struct mm_struct *check_mm_struct;

void interrupt_handler(struct trapframe *tf) {
    enum { SHUTDOWN_AFTER = 10 };
    static size_t num = 0;
    extern void sbi_shutdown(void);
    uintptr_t cause = (tf->cause << 1) >> 1;
    switch (cause) {
        case IRQ_U_SOFT:
            cprintf("User software interrupt\n");
            break;
        case IRQ_S_SOFT:
            cprintf("Supervisor software interrupt\n");
            break;
        case IRQ_H_SOFT:
            cprintf("Hypervisor software interrupt\n");
            break;
        case IRQ_M_SOFT:
            cprintf("Machine software interrupt\n");
            break;
        case IRQ_U_TIMER:
            cprintf("User Timer interrupt\n");
            break;
        case IRQ_S_TIMER:
            clock_set_next_event();
            ticks++;
            if (ticks % TICK_NUM == 0) {
                print_ticks();
                num++;
                if (num == SHUTDOWN_AFTER) {
                    sbi_shutdown();
                }
            }
            break;
        case IRQ_H_TIMER:
            cprintf("Hypervisor timer interrupt\n");
            break;
        case IRQ_M_TIMER:
            cprintf("Machine timer interrupt\n");
            break;
        case IRQ_U_EXT:
            cprintf("User external interrupt\n");
            break;
        case IRQ_S_EXT:
            cprintf("Supervisor external interrupt\n");
            break;
        case IRQ_H_EXT:
            cprintf("Hypervisor external interrupt\n");
            break;
        case IRQ_M_EXT:
            cprintf("Machine external interrupt\n");
            break;
        default:
            cprintf("Unknown interrupt: cause %lu\n", (unsigned long)cause);
            print_trapframe(tf);
            break;
    }
}

void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
        case CAUSE_MISALIGNED_FETCH:
            print_trapframe(tf);
            break;
        case CAUSE_FAULT_FETCH:
            print_trapframe(tf);
            break;
        case CAUSE_ILLEGAL_INSTRUCTION: {
            uintptr_t epc = tf->epc;
            cprintf("Illegal instruction at 0x%016lx\n", (unsigned long)epc);
            uint16_t instruction = *(const uint16_t *)epc;
            if ((instruction & 0x3) != 0x3) {
                tf->epc = epc + 2;
            } else if ((instruction & 0x1c) != 0x1c) {
                tf->epc = epc + 4;
            }
            break;
        }
        case CAUSE_BREAKPOINT: {
            uintptr_t epc = tf->epc;
            cprintf("Breakpoint at 0x%016lx\n", (unsigned long)epc);
            uint16_t instruction = *(const uint16_t *)epc;
            if ((instruction & 0x3) != 0x3) {
                tf->epc = epc + 2;
            } else if ((instruction & 0x1c) != 0x1c) {
                tf->epc = epc + 4;
            }
            break;
        }
        case CAUSE_MISALIGNED_LOAD:
            print_trapframe(tf);
            break;
        case CAUSE_FAULT_LOAD:
            print_trapframe(tf);
            break;
        case CAUSE_MISALIGNED_STORE:
            print_trapframe(tf);
            break;
        case CAUSE_FAULT_STORE:
            print_trapframe(tf);
            break;
        case CAUSE_USER_ECALL:
            print_trapframe(tf);
            break;
        case CAUSE_SUPERVISOR_ECALL:
            print_trapframe(tf);
            break;
        case CAUSE_HYPERVISOR_ECALL:
            print_trapframe(tf);
            break;
        case CAUSE_MACHINE_ECALL:
            print_trapframe(tf);
            break;
        default:
            print_trapframe(tf);
            break;
    }
}

/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap()
 * returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void trap(struct trapframe *tf)
{
    // dispatch based on what type of trap occurred
    if ((intptr_t)tf->cause < 0)
    {
        // interrupts
        interrupt_handler(tf);
    }
    else
    {
        // exceptions
        exception_handler(tf);
    }
}
