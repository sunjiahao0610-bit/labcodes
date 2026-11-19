#include <stdio.h>
#include <ulib.h>
#include <unistd.h>
#include <file.h>

#define PAGE_SIZE 4096

static int test_anonymous_mmap(void)
{
    cprintf("[1] anonymous mmap\n");

    int *p = mmap(NULL, PAGE_SIZE,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    if (p == MAP_FAILED)
    {
        cprintf("  mmap failed\n");
        return -1;
    }

    p[0] = 0x12345678;
    p[1] = 0xdeadbeef;

    if (p[0] != 0x12345678 || p[1] != 0xdeadbeef)
    {
        cprintf("  data mismatch\n");
        return -1;
    }

    munmap(p, PAGE_SIZE);
    cprintf("  ok\n");
    return 0;
}

static int test_multi_page_mmap(void)
{
    cprintf("[2] multi-page anonymous mmap\n");

    int *p = mmap(NULL, PAGE_SIZE * 4,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    if (p == MAP_FAILED)
    {
        cprintf("  mmap failed\n");
        return -1;
    }

    for (int i = 0; i < 4; i++)
        p[i * (PAGE_SIZE / sizeof(int))] = i;

    for (int i = 0; i < 4; i++)
        if (p[i * (PAGE_SIZE / sizeof(int))] != i)
        {
            cprintf("  page %d mismatch\n", i);
            return -1;
        }

    munmap(p, PAGE_SIZE * 4);
    cprintf("  ok\n");
    return 0;
}

static int test_file_mmap(void)
{
    cprintf("[3] file mmap\n");

    int fd = open("/hello", O_RDONLY);
    if (fd < 0)
    {
        cprintf("  skip (no file)\n");
        return 0;
    }

    unsigned char *p = mmap(NULL, PAGE_SIZE,
                            PROT_READ,
                            MAP_PRIVATE,
                            fd, 0);
    if (p == MAP_FAILED)
    {
        cprintf("  mmap failed\n");
        close(fd);
        return -1;
    }

    if (p[0] == 0x7f && p[1] == 'E' &&
        p[2] == 'L' && p[3] == 'F')
        cprintf("  ELF header OK\n");
    else
        cprintf("  not ELF (still OK)\n");

    munmap(p, PAGE_SIZE);
    close(fd);
    cprintf("  ok\n");
    return 0;
}

static int test_readonly_mmap(void)
{
    cprintf("[4] readonly mmap\n");

    int *p = mmap(NULL, PAGE_SIZE,
                  PROT_READ,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1, 0);
    if (p == MAP_FAILED)
    {
        cprintf("  mmap failed\n");
        return -1;
    }

    volatile int v = p[0];
    (void)v;

    munmap(p, PAGE_SIZE);
    cprintf("  ok\n");
    return 0;
}

int main(void)
{
    cprintf("==== mmap test ====\n");

    if (test_anonymous_mmap() < 0)
        goto fail;
    if (test_multi_page_mmap() < 0)
        goto fail;
    if (test_file_mmap() < 0)
        goto fail;
    if (test_readonly_mmap() < 0)
        goto fail;

    cprintf("mmaptest pass\n");
    return 0;

fail:
    cprintf("mmaptest failed\n");
    return -1;
}
