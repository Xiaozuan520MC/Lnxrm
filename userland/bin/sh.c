/* /bin/sh -- userspace terminal.
 * Fork/exec each command from /bin/. Supports > redirect. */
#include "ulib.h"

#define MAX_LINE 256
#define MAX_ARGS 16

static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    argv[argc] = 0;
    return argc;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char line[MAX_LINE];
    char pathbuf[64];

    for (;;) {
        xputs("root@lnxrm$ ");
        int pos = 0;
        for (;;) {
            char c;
            long n = kread(0, &c, 1);
            if (n <= 0) break;
            if (c == '\n' || c == '\r') {
                putc_('\n');
                break;
            }
            if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    xputs("\b \b");
                }
                continue;
            }
            if (pos < MAX_LINE - 1) {
                line[pos++] = c;
                putc_(c);
            }
        }
        line[pos] = 0;
        if (!pos) continue;

        char *args[MAX_ARGS];
        int argc = tokenize(line, args);
        if (argc == 0) continue;

        /* scan for > redirect */
        char *redir_file = 0;
        int redir_idx = -1;
        for (int i = 0; i < argc; i++) {
            if (args[i][0] == '>' && args[i][1] == 0) {
                if (i + 1 < argc) {
                    redir_file = args[i + 1];
                    redir_idx = i;
                }
                break;
            }
        }
        /* remove > and filename from args */
        if (redir_idx >= 0) {
            for (int i = redir_idx; i + 2 < argc; i++) args[i] = args[i + 2];
            argc -= 2;
            args[argc] = 0;
        }

        /* build /bin/<cmd> */
        int i = 0;
        const char *prefix = "/bin/";
        while (*prefix) pathbuf[i++] = *prefix++;
        const char *cmd = args[0];
        while (*cmd && i < 63) pathbuf[i++] = *cmd++;
        pathbuf[i] = 0;

        long pid = kfork();
        if (pid == 0) {
            if (redir_file) {
                long fd = kopen(redir_file, 0100 | 1 | 01000);
                if (fd < 0) {
                    xputs("sh: cannot open ");
                    xputs(redir_file);
                    xputs("\n");
                    kexit(1);
                }
                kdup2(fd, 1);
                kclose(fd);
            }
            if (kexecve(pathbuf, args, (char **)0) < 0) {
                xputs(args[0]);
                xputs(": command not found\n");
                kexit(1);
            }
        } else if (pid > 0) {
            int st = 0;
            kwaitpid(pid, &st, 0);
        }
    }
    return 0;
}
