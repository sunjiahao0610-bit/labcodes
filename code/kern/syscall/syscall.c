#include <unistd.h>
#include <proc.h>
#include <syscall.h>
#include <trap.h>
#include <stdio.h>
#include <pmm.h>
#include <vmm.h>
#include <assert.h>
#include <error.h>

static int
sys_exit(uint64_t arg[]) {
    int error_code = (int)arg[0];
    return do_exit(error_code);
}

static int
sys_fork(uint64_t arg[]) {
    struct trapframe *tf = current->tf;
    uintptr_t stack = tf->gpr.sp;
    return do_fork(0, stack, tf);
}

static int
sys_wait(uint64_t arg[]) {
    int pid = (int)arg[0];
    int *store = (int *)arg[1];
    return do_wait(pid, store);
}

static int
sys_exec(uint64_t arg[]) {
    const char *name = (const char *)arg[0];
    size_t len = (size_t)arg[1];
    unsigned char *binary = (unsigned char *)arg[2];
    size_t size = (size_t)arg[3];
    /*
     * Both the program name and the ELF image are supplied by the caller.
     * Check them against the current address space before do_execve() starts
     * touching them.  For kernel_execve() the current process has no user mm,
     * so user_mem_check() falls back to the kernel-address range check.
     */
    if (!user_mem_check(current->mm, (uintptr_t)name, len, 0) ||
        !user_mem_check(current->mm, (uintptr_t)binary, size, 0)) {
        return -E_INVAL;
    }
    return do_execve(name, len, binary, size);
}

static int
sys_yield(uint64_t arg[]) {
    return do_yield();
}

static int
sys_kill(uint64_t arg[]) {
    int pid = (int)arg[0];
    return do_kill(pid);
}

static int
sys_getpid(uint64_t arg[]) {
    return current->pid;
}

static int
sys_putc(uint64_t arg[]) {
    int c = (int)arg[0];
    cputchar(c);
    return 0;
}

static int
sys_pgdir(uint64_t arg[]) {
    //print_pgdir();
    return 0;
}

static int (*syscalls[])(uint64_t arg[]) = {
    [SYS_exit]              sys_exit,
    [SYS_fork]              sys_fork,
    [SYS_wait]              sys_wait,
    [SYS_exec]              sys_exec,
    [SYS_yield]             sys_yield,
    [SYS_kill]              sys_kill,
    [SYS_getpid]            sys_getpid,
    [SYS_putc]              sys_putc,
    [SYS_pgdir]             sys_pgdir,
};

#define NUM_SYSCALLS        ((sizeof(syscalls)) / (sizeof(syscalls[0])))

void
syscall(void) {
    struct trapframe *tf = current->tf;
    uint64_t arg[5];
    /*
     * The user-level ABI passes the syscall number in a0 and up to five
     * arguments in a1..a5; the return value is written back to a0.
     *
     * Keep the number as an unsigned 64-bit value: narrowing it to int
     * first would turn values such as 0x100000001 into a valid syscall
     * number and silently dispatch the wrong service.
     */
    uint64_t num = tf->gpr.a0;
    if (num < NUM_SYSCALLS && syscalls[num] != NULL) {
        arg[0] = tf->gpr.a1;
        arg[1] = tf->gpr.a2;
        arg[2] = tf->gpr.a3;
        arg[3] = tf->gpr.a4;
        arg[4] = tf->gpr.a5;
        tf->gpr.a0 = syscalls[num](arg);
        return ;
    }
    /*
     * Invalid or unimplemented syscall numbers are reported to the caller
     * instead of panicking the whole kernel; in particular, the dispatch
     * table is only indexed after the bounds check above.
     */
    tf->gpr.a0 = -E_INVAL;
}

