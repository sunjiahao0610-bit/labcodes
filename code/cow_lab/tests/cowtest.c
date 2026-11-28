#include <error.h>
#include <stdio.h>
#include <ulib.h>

#define PAGE_SIZE 4096
enum { CHILDREN = 32, ARENA_SIZE = 5 * 1024 * 1024 };

static volatile int value = 5;
static volatile int wait_status __attribute__((aligned(PAGE_SIZE)));
static const int read_only = 42;
static volatile unsigned char arena[ARENA_SIZE];

static void check_private_writes(void)
{
    volatile int stack_value = 11;
    int status;
    int pid = fork();
    if (pid == 0)
    {
        assert(value == 5 && stack_value == 11);
        value = 7;
        stack_value = 13;
        assert(value == 7 && stack_value == 13);
        exit(17);
    }
    assert(pid > 0);
    value = 19;
    assert(waitpid(pid, &status) == 0 && status == 17);
    assert(value == 19 && stack_value == 11);
    cprintf("cowtest: parent and child writes isolated\n");
}

static void check_wait_copyout(void)
{
    wait_status = -1;
    int pid = fork();
    if (pid == 0)
    {
        int worker = fork();
        if (worker == 0)
        {
            exit(23);
        }
        assert(worker > 0);
        assert(waitpid(worker, (int *)&wait_status) == 0 && wait_status == 23);
        exit(29);
    }
    assert(pid > 0);
    int status;
    assert(waitpid(pid, &status) == 0 && status == 29);
    assert(wait_status == -1);
    wait_status = 31;
    assert(wait_status == 31);
    cprintf("cowtest: wait copyout isolated, last owner writable\n");
}

static void check_read_only(void)
{
    int pid = fork();
    if (pid == 0)
    {
        /* Issue a user store without relying on a C write to a const object. */
        asm volatile("sw zero, 0(%0)" : : "r"(&read_only) : "memory");
        exit(37);
    }
    assert(pid > 0);
    assert(waitpid(pid, (int *)&read_only) == -E_INVAL);
    int status;
    assert(waitpid(pid, &status) == 0 && status == -E_FAULT);
    cprintf("cowtest: read-only writes and copyout rejected\n");
}

static void check_fork_shares_pages(void)
{
    int pids[CHILDREN];
    for (size_t offset = 0; offset < sizeof(arena); offset += PAGE_SIZE)
    {
        arena[offset] = 41;
    }
    /* 32 eager copies of 5 MiB would exceed the default QEMU RAM. */
    for (int i = 0; i < CHILDREN; i++)
    {
        int pid = fork();
        if (pid == 0)
        {
            for (;;)
            {
                yield();
            }
        }
        assert(pid > 0);
        pids[i] = pid;
    }
    for (int i = 0; i < CHILDREN; i++)
    {
        assert(kill(pids[i]) == 0);
        int status;
        assert(waitpid(pids[i], &status) == 0 && status == -E_KILLED);
    }
    assert(arena[0] == 41 && arena[sizeof(arena) - PAGE_SIZE] == 41);
    cprintf("cowtest: 32 live children share the 5 MiB region\n");
}

int main(void)
{
    check_private_writes();
    check_wait_copyout();
    check_read_only();
    check_fork_shares_pages();
    cprintf("cowtest pass.\n");
    return 0;
}
