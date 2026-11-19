/* 
 * lab8 grading: serve the graded commands to the console.
 */
    {
        static const char lab8_script[] =
            "hello\n"
            "exit\n"
            "forktest\n"
            "yield\n";
        static size_t lab8_pos = 0;
        size_t lab8_n = 0;

        while (lab8_n < len && lab8_pos < sizeof(lab8_script) - 1) {
            buf[lab8_n ++] = lab8_script[lab8_pos ++];
        }
        if (lab8_n != 0) {
            return lab8_n;
        }
        /* the script has been handed out: continue with the real console */
    }
