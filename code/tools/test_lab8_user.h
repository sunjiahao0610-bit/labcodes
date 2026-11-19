/* 
 * lab8 grading: the replacement body for user_main().
 */
{
    int kfd = sysfile_open("/canary", O_RDONLY);
    if (kfd < 0)
    {
        cprintf("[Test 1] SFS Read: FAIL (open /canary: %d)\n", kfd);
    }
    else
    {
        char kbuf[208];
        int kret = sysfile_read(kfd, kbuf, sizeof(kbuf));
        int kok = (kret == (int)sizeof(kbuf));
        if (!kok)
        {
            cprintf("[Test 1] SFS Read: FAIL (read: got %d, want %d)\n",
                    kret, (int)sizeof(kbuf));
        }
        else
        {
            int ki;
            /* /canary holds byte (i * 7 + 11) & 0xff at offset i */
            for (ki = 0; ki < (int)sizeof(kbuf); ki ++)
            {
                if ((unsigned char)kbuf[ki] != (unsigned char)((ki * 7 + 11) & 0xff))
                {
                    cprintf("[Test 1] SFS Read: FAIL (byte %d)\n", ki);
                    kok = 0;
                    break;
                }
            }
        }
        if (kok)
        {
            cprintf("[Test 1] SFS Read: PASS\n");
        }
        sysfile_close(kfd);
    }

    /* Start sh with no arguments. The graded commands reach it through the
     * console instead of through argv, because passing arguments to a user
     * program is not portable across the lab8 frameworks: some of them build
     * the argument area on the user stack without loading a0/a1, which is the
     * register pair the RISC-V user entry path reads argc/argv from. Driving sh
     * through its interactive input works on every framework and needs no
     * argument passing at all. */
    KERNEL_EXECVE(sh);
    panic("user_main execve failed.\n");
}
