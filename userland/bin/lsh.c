/* /bin/lsh -- userspace terminal.
 * Fork/exec each command from /bin/.  One line may be a pipeline of up
 * to MAX_CMDS segments joined by '|'; a segment may also read '<' FILE
 * and write '>' FILE.  File redirects are applied after the pipe ends,
 * so they win over the pipeline for that segment (POSIX: redirections
 * belong to the command, not to the pipeline).
 * Two commands are builtins instead: cd and pwd act on THIS shell's
 * working directory, which only the shell process itself can move. */
#include "ulib.h"

#define MAX_LINE 256
#define MAX_ARGS 16           /* argv slots per segment, incl. the NULL */
#define MAX_CMDS 4            /* segments in one pipeline */
#define MAX_TOKS (MAX_CMDS * MAX_ARGS)

/* Split the line on blanks.  The token table is now flat across the whole
 * line -- every segment's argv is carved out of it by split_pipeline. */
static int tokenize(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    tok[n] = 0;
    return n;
}

/* Carve the token stream into segments.  '|' starts a new one; '<' and '>'
 * (exactly one character each -- the tokenizer only splits on blanks, so
 * 'a|b' and '>>' stay ordinary words) peel their file name off and leave
 * the segment's argv, last one of a kind winning.  Returns the segment
 * count, or 0 with *why set to the complaint for the caller to print. */
static int split_pipeline(char **tok, int ntok, char *argvs[][MAX_ARGS],
                          char **in, char **out, const char **why)
{
    int seg = 0;
    int n = 0;

    for (int i = 0; i < MAX_CMDS; i++) {
        in[i] = 0;
        out[i] = 0;
    }

    for (int i = 0; i < ntok; i++) {
        char *t = tok[i];

        if (t[0] == '|' && t[1] == 0) {
            if (n == 0) {
                *why = "syntax error: empty command before '|'";
                return 0;
            }
            if (seg + 1 >= MAX_CMDS) {
                *why = "pipeline longer than 4 commands";
                return 0;
            }
            argvs[seg][n] = 0;
            seg++;
            n = 0;
            continue;
        }
        if ((t[0] == '<' || t[0] == '>') && t[1] == 0) {
            int want_in = t[0] == '<';
            if (i + 1 >= ntok || (tok[i + 1][0] == '|' && tok[i + 1][1] == 0)) {
                *why = "syntax error: missing file after redirect";
                return 0;
            }
            i++;
            if (want_in)
                in[seg] = tok[i];
            else
                out[seg] = tok[i];
            continue;
        }
        if (n >= MAX_ARGS - 1) {
            *why = "too many arguments";
            return 0;
        }
        argvs[seg][n++] = t;
    }

    if (n == 0) {
        *why = "syntax error: empty command after '|'";
        return 0;
    }
    argvs[seg][n] = 0;
    return seg + 1;
}

/* Run one parsed line.  Every segment is forked BEFORE any of them is
 * waited for: waiting in the middle would let the first writer fill its
 * 4096-byte ring and park against a reader that does not exist yet. */
static void run_pipeline(char *argvs[][MAX_ARGS], char **in, char **out,
                         int ncmds)
{
    int pfd[MAX_CMDS - 1][2];
    long pids[MAX_CMDS];
    int npipes = ncmds - 1;

    for (int i = 0; i < ncmds; i++)
        pids[i] = -1;

    for (int i = 0; i < npipes; i++) {
        if (kpipe(pfd[i]) < 0) {
            xputs("sh: pipe failed\n");
            for (int j = 0; j < i; j++) {
                kclose(pfd[j][0]);
                kclose(pfd[j][1]);
            }
            return;
        }
    }

    for (int i = 0; i < ncmds; i++) {
        long pid = kfork();
        if (pid == 0) {
            /* wire this segment into the pipeline first ... */
            if (i > 0 && kdup2(pfd[i - 1][0], 0) < 0) {
                xputs("sh: dup2 failed\n");
                kexit(1);
            }
            if (i + 1 < ncmds && kdup2(pfd[i][1], 1) < 0) {
                xputs("sh: dup2 failed\n");
                kexit(1);
            }
            /* ... then drop EVERY end, including the two just duplicated.
             * A child that keeps a foreign end alive holds the ring open:
             * its own reader would wait for an EOF that only it prevents
             * (and the next writer would see EPIPE from nobody). */
            for (int j = 0; j < npipes; j++) {
                kclose(pfd[j][0]);
                kclose(pfd[j][1]);
            }
            /* File redirects last, so '<'/'>' replace whatever the
             * pipeline put on fd 0/fd 1 for this segment only. */
            if (in[i]) {
                long fd = kopen(in[i], 0);
                if (fd < 0) {
                    xputs("sh: cannot open ");
                    xputs(in[i]);
                    xputs("\n");
                    kexit(1);
                }
                kdup2(fd, 0);
                kclose(fd);
            }
            if (out[i]) {
                long fd = kopen(out[i], 0100 | 1 | 01000); /* CREAT|WRONLY|TRUNC */
                if (fd < 0) {
                    xputs("sh: cannot open ");
                    xputs(out[i]);
                    xputs("\n");
                    kexit(1);
                }
                kdup2(fd, 1);
                kclose(fd);
            }

            /* build /bin/<cmd> */
            char pathbuf[64];
            int k = 0;
            const char *prefix = "/bin/";
            while (*prefix && k < 63) pathbuf[k++] = *prefix++;
            const char *cmd = argvs[i][0];
            while (*cmd && k < 63) pathbuf[k++] = *cmd++;
            pathbuf[k] = 0;

            if (kexecve(pathbuf, argvs[i], (char **)0) < 0) {
                xputs(argvs[i][0]);
                xputs(": command not found\n");
            }
            kexit(1);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            xputs("sh: fork failed\n");
        }
    }

    /* The parent's copies are the last reference keeping the rings open:
     * drop them before waiting or the first reader blocks on EOF. */
    for (int j = 0; j < npipes; j++) {
        kclose(pfd[j][0]);
        kclose(pfd[j][1]);
    }
    for (int i = 0; i < ncmds; i++) {
        if (pids[i] > 0) {
            int st = 0;
            kwaitpid(pids[i], &st, 0);
        }
    }
}

/* ulib ships no strcmp: exact match is all the builtin names need. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* cd [dir]: move the shell's own cwd (chdir 49).  No argument goes home
 * to "/" -- this shell has no $HOME to consult.  On failure nothing is
 * printed as changed either: the kernel writes its cwd field only after
 * the target has proved to be an existing directory, so there is no
 * half-done state to undo here. */
static void builtin_cd(char **argv)
{
    int n = 0;
    while (argv[n]) n++;
    if (n > 2) {
        xputs("cd: too many arguments\n");
        return;
    }
    const char *dir = n == 2 ? argv[1] : "/";
    long r = kchdir(dir);
    if (r < 0) {
        xputs("cd: '");
        xputs(dir);
        xputs("': ");
        if (r == LNXRM_ENOENT)
            xputs("no such directory\n");
        else if (r == LNXRM_ENOTDIR)
            xputs("not a directory\n");
        else if (r == LNXRM_ENAMETOOLONG)
            xputs("name too long\n");
        else
            xputs("failed\n");
    }
}

/* pwd: ask the kernel (getcwd 50) rather than keep a copy here, so it
 * can never disagree with what cd actually did. */
static void builtin_pwd(char **argv)
{
    char buf[128];

    (void)argv;
    if (kgetcwd(buf, sizeof(buf)) < 0) {
        xputs("pwd: failed\n");
        return;
    }
    xputs(buf);
    xputs("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char line[MAX_LINE];
    char *toks[MAX_TOKS];

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

        int ntok = tokenize(line, toks, MAX_TOKS);
        if (ntok == 0) continue;

        char *argvs[MAX_CMDS][MAX_ARGS];
        char *in[MAX_CMDS], *out[MAX_CMDS];
        const char *why = 0;
        int ncmds = split_pipeline(toks, ntok, argvs, in, out, &why);
        if (!ncmds) {
            xputs("sh: ");
            xputs(why);
            xputs("\n");
            continue;
        }

        /* Builtins first: cd/pwd must run in THIS process to mean
         * anything, and only for a plain one-command line.  Piped or
         * redirected they fall through to exec like any other name --
         * a cd inside a pipeline could only move a subshell nobody
         * watches, and pretending otherwise would hide the fact. */
        if (ncmds == 1 && !in[0] && !out[0]) {
            if (streq(argvs[0][0], "cd")) {
                builtin_cd(argvs[0]);
                continue;
            }
            if (streq(argvs[0][0], "pwd")) {
                builtin_pwd(argvs[0]);
                continue;
            }
        }

        run_pipeline(argvs, in, out, ncmds);
    }
    return 0;
}
