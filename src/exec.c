/*
 * Running a parsed pipeline: fork a child per command, connect them with
 * pipes, apply redirections with dup2, exec, and wait.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shell.h"

static int decode_status(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 1;
}

/* Opens each redirection target and moves it onto fd 0 or 1. Later
 * redirections win, so `cmd > a > b` writes to b (a is still created). */
static int apply_redirs(const Command *cmd)
{
    for (size_t i = 0; i < cmd->nredirs; i++) {
        const Redir *r = &cmd->redirs[i];
        int flags, target;
        switch (r->kind) {
        case REDIR_IN:
            flags = O_RDONLY;
            target = STDIN_FILENO;
            break;
        case REDIR_OUT:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            target = STDOUT_FILENO;
            break;
        default:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            target = STDOUT_FILENO;
            break;
        }
        int fd = open(r->path, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "mini-shell: %s: %s\n", r->path, strerror(errno));
            return -1;
        }
        if (fd != target) {
            if (dup2(fd, target) < 0) {
                fprintf(stderr, "mini-shell: dup2: %s\n", strerror(errno));
                close(fd);
                return -1;
            }
            close(fd);
        }
    }
    return 0;
}

/*
 * A built-in on its own runs in the shell process, so `cd` and `export`
 * actually change the shell. If it has redirections, stdin/stdout are saved
 * first and put back afterwards.
 */
static int run_builtin_in_parent(Shell *sh, Command *cmd)
{
    if (cmd->nredirs == 0)
        return run_builtin(sh, cmd);

    fflush(stdout);
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    if (saved_in < 0 || saved_out < 0) {
        fprintf(stderr, "mini-shell: dup: %s\n", strerror(errno));
        if (saved_in >= 0)
            close(saved_in);
        if (saved_out >= 0)
            close(saved_out);
        return 1;
    }
    int status = 1;
    if (apply_redirs(cmd) == 0)
        status = run_builtin(sh, cmd);
    fflush(stdout); /* before stdout goes back to where it was */
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
    return status;
}

/* Runs in the child after fork. Never returns. */
static void child(Shell *sh, Command *cmd, int in_fd, int out_fd, int unused_fd,
                  int background, pid_t pgid)
{
    /* The shell ignores or catches these; the program should not. */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);

    if (background) {
        /* Own process group, so a Ctrl-C meant for the foreground does not
         * reach it. */
        setpgid(0, pgid);
        /* Without job control a background job must not read the
         * terminal (or the script the shell is reading). */
        if (in_fd < 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
    }

    if (in_fd >= 0) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
    }
    if (out_fd >= 0) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
    /* The read end of the pipe this child writes to belongs to the next
     * command. If it stayed open here, nothing would change, but every
     * extra copy of a pipe end is a chance for a reader to never see EOF. */
    if (unused_fd >= 0)
        close(unused_fd);

    if (apply_redirs(cmd) != 0)
        _exit(1);

    if (is_builtin(cmd->argv[0])) {
        int status = run_builtin(sh, cmd);
        fflush(stdout);
        _exit(status);
    }

    execvp(cmd->argv[0], cmd->argv);
    int err = errno;
    if (err == ENOENT)
        fprintf(stderr, "mini-shell: %s: command not found\n", cmd->argv[0]);
    else
        fprintf(stderr, "mini-shell: %s: %s\n", cmd->argv[0], strerror(err));
    _exit(err == ENOENT ? 127 : 126);
}

static void wait_for(pid_t pid, int *st)
{
    /* SIGINT interrupts waitpid (no SA_RESTART), so try again. */
    while (waitpid(pid, st, 0) < 0) {
        if (errno != EINTR) {
            *st = 1 << 8; /* treat as exit status 1 */
            return;
        }
    }
}

/* Remembers a background pipeline so it can be reaped and reported later.
 * Takes ownership of pids. */
static void add_job(Shell *sh, pid_t *pids, size_t n, const char *line)
{
    Job *jobs = realloc(sh->jobs, (sh->njobs + 1) * sizeof *jobs);
    if (jobs == NULL) {
        /* The processes still run; they just won't be listed. */
        free(pids);
        return;
    }
    sh->jobs = jobs;

    /* job text: the line without the trailing & */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    if (len > 0 && line[len - 1] == '&')
        len--;
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    char *text = malloc(len + 1);
    if (text != NULL) {
        memcpy(text, line, len);
        text[len] = '\0';
    }

    int id = 1;
    for (size_t i = 0; i < sh->njobs; i++)
        if (sh->jobs[i].id >= id)
            id = sh->jobs[i].id + 1;

    Job *j = &sh->jobs[sh->njobs++];
    j->id = id;
    j->pids = pids;
    j->npids = n;
    j->alive = n;
    j->text = text;
    if (sh->interactive)
        printf("[%d] %ld\n", id, (long)pids[n - 1]);
}

int run_pipeline(Shell *sh, Pipeline *p, const char *line)
{
    if (p->ncmds == 1 && !p->background && is_builtin(p->cmds[0].argv[0]))
        return run_builtin_in_parent(sh, &p->cmds[0]);

    pid_t *pids = calloc(p->ncmds, sizeof *pids);
    if (pids == NULL) {
        fprintf(stderr, "mini-shell: out of memory\n");
        return 1;
    }

    /*
     * For `a | b | c`, with two pipes P1 and P2:
     *   a: stdout -> P1 write end
     *   b: stdin  <- P1 read end,  stdout -> P2 write end
     *   c: stdin  <- P2 read end
     * `prev_read` carries the read end of the previous pipe to the next
     * command. The parent closes each end as soon as the children that need
     * it have been forked; otherwise `c` would never see end-of-file.
     */
    size_t started = 0;
    int prev_read = -1;
    int failed = 0;
    pid_t pgid = 0;
    for (size_t i = 0; i < p->ncmds; i++) {
        int fds[2] = {-1, -1};
        if (i + 1 < p->ncmds && pipe(fds) < 0) {
            fprintf(stderr, "mini-shell: pipe: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        /* Anything still in our stdio buffers would otherwise be copied
         * into the child and printed twice. */
        fflush(stdout);
        fflush(stderr);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "mini-shell: fork: %s\n", strerror(errno));
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            failed = 1;
            break;
        }
        if (pid == 0)
            child(sh, &p->cmds[i], prev_read, fds[1], fds[0], p->background, pgid);

        if (p->background) {
            /* Also set the group here, in case the parent gets to run
             * before the child has called setpgid itself. */
            if (pgid == 0)
                pgid = pid;
            setpgid(pid, pgid);
        }
        pids[started++] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        close(prev_read);

    if (p->background && started > 0 && !failed) {
        add_job(sh, pids, started, line);
        return 0;
    }

    int status = 1;
    for (size_t i = 0; i < started; i++) {
        int st;
        wait_for(pids[i], &st);
        if (i == started - 1)
            status = decode_status(st);
    }
    free(pids);
    if (failed)
        return 1;
    if (status == 128 + SIGINT && sh->interactive)
        printf("\n"); /* the ^C is on the prompt line otherwise */
    return status;
}

void reap_jobs(Shell *sh)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        for (size_t i = 0; i < sh->njobs; i++) {
            Job *j = &sh->jobs[i];
            int found = 0;
            for (size_t k = 0; k < j->npids; k++)
                if (j->pids[k] == pid)
                    found = 1;
            if (!found)
                continue;
            if (--j->alive == 0) {
                if (sh->interactive)
                    printf("[%d] Done       %s\n", j->id, j->text ? j->text : "");
                free(j->pids);
                free(j->text);
                /* keep the rest in order for `jobs` */
                memmove(&sh->jobs[i], &sh->jobs[i + 1],
                        (sh->njobs - i - 1) * sizeof *sh->jobs);
                sh->njobs--;
            }
            break;
        }
    }
}

void free_jobs(Shell *sh)
{
    for (size_t i = 0; i < sh->njobs; i++) {
        free(sh->jobs[i].pids);
        free(sh->jobs[i].text);
    }
    free(sh->jobs);
    sh->jobs = NULL;
    sh->njobs = 0;
}
